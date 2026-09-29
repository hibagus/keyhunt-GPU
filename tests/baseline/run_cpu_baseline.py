#!/usr/bin/env python3
"""Characterize the original CPU CLI, including explicitly named legacy defects.

Uses only Python's standard library and committed synthetic public vectors.
Every subprocess runs in an isolated directory and has a hard time limit.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

DATA = json.loads(Path(__file__).with_name('vectors.json').read_text())
VECTORS = DATA['vectors']
KEY_PATTERN = re.compile(r'(?:Private Key:|Key found privkey)\s+([0-9a-fA-F]+)')


def key_list(text):
    return [format(int(k, 16), 'x') for k in KEY_PATTERN.findall(text)]


def execute(binary, args, targets='', timeout=15.0):
    with tempfile.TemporaryDirectory(prefix='keyhunt-baseline-') as folder:
        cwd = Path(folder)
        (cwd / 'targets.txt').write_text(targets)
        start = time.perf_counter()
        timed_out = False
        try:
            result = subprocess.run([str(binary), *args], cwd=cwd,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout)
            code, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired as error:
            # subprocess.run kills and waits for the child before raising.
            timed_out, code, output = True, None, error.stdout or b''
        elapsed = time.perf_counter() - start
        artifacts = {p.name: p.read_text(errors='replace') for p in cwd.iterdir()
                     if p.name in ('KEYFOUNDKEYFOUND.txt', 'VANITYKEYFOUND.txt')}
        return {'argv': args, 'exit_code': code, 'timed_out': timed_out,
                'wall_seconds': elapsed, 'output': output.decode(errors='replace'),
                'results': artifacts}


def search_args(mode, bounds='1000:1400', n='1024', extra=()):
    return ['-m', mode, '-f', 'targets.txt', '-r', bounds, '-n', n,
            '-t', '1', '-q', '-s', '0', *extra]


def targets(keys, field='x', encoding='compressed'):
    return ''.join((VECTORS[k][field] if field == 'x'
                    else VECTORS[k][encoding][field]) + '\n' for k in keys)


def cases():
    checks = []

    def add(name, args, data='', keys=(), code=0, contains=(), timeout=15.0,
            timed_out=False, result_contains=(), known_defect=False):
        checks.append(dict(name=name, args=args, data=data, keys=list(keys), code=code,
                           contains=list(contains), timeout=timeout, timed_out=timed_out,
                           result_contains=list(result_contains), known_defect=known_defect))

    add('help_existing_exit_status', ['-h'], code=1, contains=['Usage:', '-m mode'])
    add('invalid_mode', ['-m', 'invalid'], code=1, contains=['Unknow mode value'])
    add('invalid_crypto', ['-c', 'invalid'], code=1, contains=['Unknow crypto value'])
    add('unknown_option', ['-?'], code=1, contains=['Unknow opcion'])
    add('pub2rmd_removed', ['-m', 'pub2rmd'], contains=['Mode pub2rmd was removed'])
    add('missing_input', search_args('xpoint') + ['-f', 'missing.txt'],
        code=1, contains=["Error opening the file"])
    add('empty_input_accepted_existing_defect', search_args('xpoint'),
        contains=['0 values were loaded', 'End'], known_defect=True)
    add('malformed_xpoint_ignored_existing_defect', search_args('xpoint'), 'not-a-point\n',
        contains=['0 values were loaded', 'End'], known_defect=True)
    boundary = ['fff', '1000', '1001', '13ff', '1400', '1401', '17ff', '1800']
    add('xpoint_aligned_boundary', search_args('xpoint'), targets(boundary),
        keys=['1000', '1001', '13ff'], contains=['End'])
    add('xpoint_tail_overrun_existing_defect', search_args('xpoint', '1000:1401'),
        targets(boundary), keys=['1000', '1001', '13ff', '1400', '1401', '17ff'],
        known_defect=True)
    add('xpoint_tiny_range_overrun_existing_defect', search_args('xpoint', '1000:1001'),
        targets(boundary), keys=['1000', '1001', '13ff'], known_defect=True)
    add('xpoint_no_match', search_args('xpoint'), targets(['fff', '1400']), contains=['End'])
    add('xpoint_swapped_range', search_args('xpoint', '1400:1000'),
        targets(boundary), keys=['1000', '1001', '13ff'], contains=['Swapping them'])
    add('zero_start_becomes_one', search_args('xpoint', '0:401'),
        targets(['1', '2', '3']), keys=['1', '2', '3'], contains=['from : 0x1'])
    high = [format((1 << 80) + x, 'x') for x in [0x1000, 0x13ff, 0x1400]]
    add('xpoint_above_64_bits', search_args('xpoint', high[0] + ':' + high[-1]),
        targets(high), keys=high[:2])
    stride = ['1000', '1001', '1002', '13ff', '1400', '1401', '17fe', '17ff',
              '1800', '1801', '1bfe', '1bff']
    add('stride_overlapping_batches_existing_defect',
        search_args('xpoint', '1000:1800', extra=['-I', '2']), targets(stride),
        keys=['1000', '1002', '1400', '17fe', '1400', '17fe', '1800', '1bfe'],
        known_defect=True)
    for mode, field in [('address', 'address'), ('rmd160', 'hash160')]:
        for encoding, flag in [('compressed', 'compress'), ('uncompressed', 'uncompress')]:
            add(mode + '_' + encoding, search_args(mode, extra=['-l', flag]),
                targets(boundary, field, encoding), keys=['1000', '1001', '13ff'],
                result_contains=[VECTORS[k][encoding]['public_key'] for k in ['1000', '1001', '13ff']])
        add(mode + '_both', search_args(mode, extra=['-l', 'both']),
            targets(['1000'], field) + targets(['1000'], field, 'uncompressed'),
            keys=['1000', '1000'], result_contains=[VECTORS['1000'][e]['public_key']
                                                  for e in ['compressed', 'uncompressed']])
        add(mode + '_no_match', search_args(mode), targets(['fff', '1400'], field))
    add('ethereum_address', search_args('address', '1:401', extra=['-c', 'eth']),
        DATA['ethereum_scalar_1'] + '\n', keys=['1'], result_contains=[DATA['ethereum_scalar_1']])
    add('vanity_prefix', search_args('vanity', '1:401', extra=['-l', 'compress', '-v', '1BgGZ9tc']),
        keys=['1'], result_contains=[VECTORS['1']['compressed']['address']])
    add('vanity_no_match', search_args('vanity', extra=['-l', 'compress', '-v', '1BgGZ9tc']))
    add('minikey_deterministic_start', search_args('minikeys', extra=['-C', DATA['minikey']['base']]),
        DATA['minikey']['uncompressed']['address'] + '\n', keys=[DATA['minikey']['scalar']],
        timeout=2.0, timed_out=True, code=None, result_contains=[DATA['minikey']['value']])
    add('minikey_invalid_length', ['-m', 'minikeys', '-C', 'short'], code=1,
        contains=['Invalid Minikey length'])
    add('random_mode_does_not_exhaust', search_args('xpoint', extra=['-R', '-s', '1']),
        targets(['1']), timeout=2.0, timed_out=True, code=None, contains=['Random mode'])
    add('equal_endpoints_fallback_existing_defect',
        search_args('xpoint', '1000:1000') + ['-f', 'missing.txt'], code=1,
        contains=["Start and End range can't be the same", 'Fallback to random mode!',
                  'from : 0x1'], known_defect=True)
    bsgs_args = search_args('bsgs', '100000:300000', '1048576')
    add('bsgs_known_match_compressed', bsgs_args, targets(['100001'], 'public_key'),
        keys=['100001'], code=1, contains=['All points were found'],
        result_contains=[VECTORS['100001']['compressed']['public_key']])
    add('bsgs_known_match_uncompressed', bsgs_args,
        targets(['100001'], 'public_key', 'uncompressed'), keys=['100001'], code=1,
        contains=['All points were found'], result_contains=[VECTORS['100001']['uncompressed']['public_key']])
    add('bsgs_no_match', bsgs_args, targets(['400000'], 'public_key'), contains=['End'])
    add('bsgs_start_missed_existing_defect', bsgs_args, targets(['100000'], 'public_key'),
        contains=['End'], known_defect=True)
    add('bsgs_tail_overrun_existing_defect', search_args('bsgs', '100000:200001', '1048576'),
        targets(['100000', '100001', '100002', '200000', '200001', '200002', '300000', '300001'], 'public_key'),
        keys=['100001', '100002', '200000', '200001', '200002', '300000'], known_defect=True)
    add('bsgs_invalid_n', search_args('bsgs', n='1025'), targets(['1'], 'public_key'),
        code=1, contains=["doesn't have exact square root"])
    add('bsgs_range_too_small', search_args('bsgs', n='1048576'), targets(['1'], 'public_key'),
        code=1, contains=['the given range is small'])
    return checks


def check(case, result):
    failures = []
    if result['exit_code'] != case['code'] or result['timed_out'] != case['timed_out']:
        failures.append(f"expected exit={case['code']} timeout={case['timed_out']}, "
                        f"got exit={result['exit_code']} timeout={result['timed_out']}")
    found = '\n'.join(result['results'].values())
    if Counter(key_list(found)) != Counter(case['keys']):
        failures.append(f"expected result keys {case['keys']}, got {key_list(found)}")
    # Timeout may kill the original CLI before stdio buffers flush. Durable
    # result files still must contain exactly the expected known test matches.
    if not case['timed_out'] and Counter(key_list(result['output'])) != Counter(case['keys']):
        failures.append(f"stdout keys differ: {key_list(result['output'])}")
    for fragment in case['contains']:
        if fragment not in result['output']:
            failures.append(f"missing output: {fragment!r}")
    for fragment in case['result_contains']:
        if fragment not in found:
            failures.append(f"missing result: {fragment!r}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--case', help='Run names containing this substring')
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    records = []
    selected = [case for case in cases() if not args.case or args.case in case['name']]
    if not selected:
        parser.error('no matching cases')
    for case in selected:
        result = execute(binary, case['args'], case['data'], case['timeout'])
        failures = check(case, result)
        record = dict(name=case['name'], known_defect=case['known_defect'],
                      passed=not failures, failures=failures, **result)
        records.append(record)
        print(('PASS' if not failures else 'FAIL') + ' ' + case['name'], flush=True)
        if failures:
            print('\n'.join(failures) + '\n' + result['output'], flush=True)
    report = {'schema_version': 1, 'captured_at': datetime.now(timezone.utc).isoformat(),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'fixture_sha256': hashlib.sha256(Path(__file__).with_name('vectors.json').read_bytes()).hexdigest(),
              'cases': records}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    count = sum(r['passed'] for r in records)
    print(f'{count}/{len(records)} baseline cases passed')
    return 0 if count == len(records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
