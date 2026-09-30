#!/usr/bin/env python3
"""Audit actual CPU search boundaries and additional random point operations.

Uses synthetic public targets from the pinned C06 oracle, checked against its
independent Python model. Searches run in temporary directories with timeouts.
Returns 1 if any exact expected search result or point operation is missing/wrong.
This audit deliberately detects inherited search defects outside the C06 suites.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/oracle'))
from model import P, N, add, encode, multiply
from oracle_selftest import check_source, run


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def search_cases(binary, oracle):
    cases = [
        ('xpoint_zero_denominator', 'xpoint', 0x200, 0x600, 1024,
         [0x200, 0x201, 0x3ff, 0x400, 0x401, 0x5ff]),
        ('xpoint_short_order_tail', 'xpoint', N-19, N, 1024, [N-19, N-2, N-1]),
        ('xpoint_aligned_order_tail', 'xpoint', N-1024, N, 1024,
         [N-1024, N-19, N-2, N-1]),
        ('xpoint_partial_order_tail', 'xpoint', N-1025, N, 1024, [N-1025, N-2, N-1]),
        ('bsgs_step_boundaries', 'bsgs', 0x100000, 0x300000, 1048576,
         [0x100000+x for x in (0, 1, 1023, 1024, 1025, 2047, 2048, 2049,
                               3072, 1049599, 1049600, 1049601)]),
    ]
    scalars = sorted({k for _, _, _, _, _, keys in cases for k in keys})
    pubs = dict(zip(scalars, run(oracle, [f'pub {k:064x}' for k in scalars])))
    if any(pub != encode(multiply(k)) for k, pub in pubs.items()):
        raise RuntimeError('independent public-key oracles disagree')
    results = []
    for name, mode, begin, end, batch, keys in cases:
        targets = ''.join((pubs[k][2:66] if mode == 'xpoint' else pubs[k]) + '\n'
                          for k in keys)
        argv = ['-m', mode, '-f', 'targets.txt', '-r', f'{begin:x}:{end:x}',
                '-n', str(batch), '-t', '1', '-q', '-s', '0']
        with tempfile.TemporaryDirectory(prefix='keyhunt-c06-boundary-') as folder:
            cwd = Path(folder)
            (cwd / 'targets.txt').write_text(targets)
            timed_out = False
            try:
                execution = subprocess.run([str(binary), *argv], cwd=cwd,
                                           capture_output=True, text=True, timeout=20)
                code, stdout, stderr = execution.returncode, execution.stdout, execution.stderr
            except subprocess.TimeoutExpired as error:
                timed_out, code = True, None
                stdout = (error.stdout or b'').decode(errors='replace')
                stderr = (error.stderr or b'').decode(errors='replace')
            found_path = cwd / 'KEYFOUNDKEYFOUND.txt'
            found_text = found_path.read_text() if found_path.exists() else ''
        found = [format(int(k, 16), 'x') for k in re.findall(
            r'(?:Private Key:|Key found privkey)\s+([0-9a-fA-F]+)', found_text)]
        expected = [f'{k:x}' for k in keys]
        # The existing BSGS CLI exits 1 when it finds every supplied target.
        expected_exit = 1 if mode == 'bsgs' else 0
        passed = not timed_out and code == expected_exit and Counter(found) == Counter(expected)
        results.append({'name': name, 'argv': argv, 'targets': targets,
                        'expected': expected, 'found': found,
                        'missing': list((Counter(expected)-Counter(found)).elements()),
                        'unexpected': list((Counter(found)-Counter(expected)).elements()),
                        'passed': passed, 'expected_exit': expected_exit,
                        'exit_code': code, 'timed_out': timed_out,
                        'stdout': stdout, 'stderr': stderr, 'result_file': found_text})
        print(f'{name}: {len(found)}/{len(expected)} matches; passed={passed}', flush=True)
    return {'binary_sha256': digest(binary), 'cases': results}


def extended_points(binary, oracle):
    seed = 0xA06D1FF
    rng = random.Random(seed)
    scalars = [rng.randrange(1, N) for _ in range(128)]
    pubs = run(oracle, [f'pub {s:064x}' for s in scalars])
    points = [(int(pub[2:66], 16), int(pub[66:], 16)) for pub in pubs]
    if any(point != multiply(s) for point, s in zip(points, scalars)):
        raise RuntimeError('independent point oracles disagree')
    pairs = [tuple(rng.sample(range(128), 2)) for _ in range(256)]
    sums = run(oracle, [f'add {pubs[a]} {pubs[b]}' for a, b in pairs])
    cases = []
    for (a, b), expected in zip(pairs, sums):
        if expected != encode(add(points[a], points[b])):
            raise RuntimeError('independent addition oracles disagree')
        za, zb = rng.randrange(1, P), rng.randrange(1, P)
        for op in ('padd', 'padd2', 'padd_direct'):
            cases.append((f'{op} {pubs[a]} {pubs[b]} {za:064x} {zb:064x}',
                          ' '.join([expected]*3)))
    for pub, point in zip(pubs, points):
        z = rng.randrange(1, P)
        for op in ('pdouble', 'pdouble_direct'):
            cases.append((f'{op} {pub} {z:064x}', ' '.join([encode(add(point, point))]*2)))
        cases.append((f'preduce {pub} {z:064x}', pub))
    actual = run(binary, [command for command, _ in cases])
    failures = [{'command': command, 'expected': expected, 'actual': observed}
                for (command, expected), observed in zip(cases, actual) if observed != expected]
    print(f'Extended point checks: {len(cases)}; failures={len(failures)}', flush=True)
    return {'seed': seed, 'distinct_random_points': 128, 'random_pair_count': 256,
            'cases': len(cases), 'failure_count': len(failures), 'failures': failures[:10],
            'binary_sha256': digest(binary)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--arithmetic', type=Path, required=True)
    parser.add_argument('--previous-binary', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = {'schema_version': 1, 'oracle_commit': check_source(),
              'oracle_binary_sha256': digest(args.oracle),
              'scope': 'Synthetic CPU search and arithmetic audit; no GPU validation',
              'current_search': search_cases(args.binary.resolve(strict=True), args.oracle),
              'extended_points': extended_points(args.arithmetic.resolve(strict=True), args.oracle)}
    if args.previous_binary:
        report['previous_search'] = search_cases(args.previous_binary.resolve(strict=True), args.oracle)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Report: {args.report}')
    return int(any(not case['passed'] for case in report['current_search']['cases'])
               or report['extended_points']['failure_count'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
