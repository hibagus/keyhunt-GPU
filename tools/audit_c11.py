#!/usr/bin/env python3
"""Replay the C06 misses on HIP and measure bounded C11 performance probes.

Uses synthetic targets and temporary files. Honors the caller's GPU visibility;
select HIP_VISIBLE_DEVICES=0 externally for a single-device audit. No GPU settings
are changed. Optional baseline/variant benchmarks must be built separately.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(argv, timeout=120):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True,
                            timeout=timeout)
    if result.returncode or result.stderr:
        raise RuntimeError(f'{argv}: exit={result.returncode}: {result.stderr}')
    return result.stdout


def inspect_search(binary, mode, arguments, begin, end, expected, capacity):
    stdout = run([binary, mode, '--backend', 'hip', '--device', '0', *arguments])
    records = [json.loads(line) for line in stdout.splitlines()]
    start, summary = records[0], records[-1]
    assert start['type'] == 'start' and summary['type'] == 'summary'
    assert not start['durable_coverage'] and not summary['durable_coverage']
    cursor, first, attempted, verified, overflows = begin, 0, 0, 0, 0
    found, batches = [], []
    for record in records[1:-1]:
        assert int(record['begin'], 16) == cursor
        limit = int(record['end_exclusive'], 16)
        assert cursor < limit <= end
        if record['type'] == 'tile':
            assert mode == 'bsgs' and first == start['target_count']
            assert record['targets_completed'] == first
            cursor, first = limit, 0
            continue
        assert record['type'] == 'batch'
        batches.append(record)
        if mode == 'xpoint':
            assert record['device_steps'] == limit - cursor
        else:
            assert record['first_target'] == first
            assert record['giants_per_target'] == (limit-cursor+start['m']-1)//start['m']
            assert record['device_steps'] == record['giants_per_target']*record['target_count']
        attempted += record['device_steps']
        if record['overflow']:
            assert record['candidate_count'] > capacity
            assert record['verified_steps'] == 0 and record['matches'] == []
            overflows += 1
        else:
            assert record['candidate_count'] == len(record['matches']) <= capacity
            assert record['verified_steps'] == record['device_steps']
            verified += record['verified_steps']
            found.extend(int(match['scalar'], 16) for match in record['matches'])
            if mode == 'xpoint':
                cursor = limit
            else:
                first += record['target_count']
    assert cursor == end and sorted(found) == sorted(expected)
    assert summary['complete'] and int(summary['device_steps'], 16) == attempted
    assert summary['launch_count'] == len(batches) and summary['overflow_replays'] == overflows
    assert int(summary['matches'], 16) == len(expected)
    if mode == 'xpoint':
        assert int(summary['verified_steps'], 16) == verified == end-begin
    else:
        assert int(summary['verified_scalars'], 16) == end-begin
        assert int(summary['verified_target_steps'], 16) == verified
    warm = batches[1:] or batches
    metrics = {key: statistics.median(b[key] for b in warm)
               for key in ('kernel_ms', 'download_ms', 'wall_ms', 'download_bytes')}
    return {'arguments': list(map(str, arguments)), 'passed': True,
            'expected': [f'{value:x}' for value in expected],
            'found': [f'{value:x}' for value in sorted(found)], 'start': start,
            'summary': summary, 'warm_batch_medians': metrics,
            'batch_step_histogram': dict(Counter(b['device_steps'] for b in batches)),
            'stdout_sha256': hashlib.sha256(stdout.encode()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--previous-report', type=Path,
                        default=ROOT/'docs/audits/C06_AUDIT_PROBES.json')
    parser.add_argument('--benchmark', type=Path)
    parser.add_argument('--variant-benchmark', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if bool(args.benchmark) != bool(args.variant_benchmark):
        parser.error('provide both benchmark paths or neither')
    binary = args.binary.resolve(strict=True)
    previous = json.loads(args.previous_report.read_text())
    cases = previous['current_search']['cases']
    report = {'schema_version': 1, 'recorded_utc': datetime.now(timezone.utc).isoformat(),
              'binary_sha256': sha(binary), 'oracle_binary_sha256': sha(args.oracle),
              'prior_evidence_sha256': sha(args.previous_report),
              'oracle_commit': previous['oracle_commit'],
              'visibility': {k: os.environ.get(k) for k in
                             ('HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES')},
              'inventory': json.loads(run([binary, 'devices', '--backend', 'hip'])),
              'regressions': [], 'overflow_performance': [], 'capacity_performance': [],
              'launch_bound_benchmarks': []}
    keys = sorted({int(k, 16) for c in cases for k in c['expected']} | {1, 2})
    oracle = subprocess.run([str(args.oracle.resolve())],
                            input=''.join(f'pub {k:064x}\n' for k in keys),
                            text=True, capture_output=True, check=True, timeout=60)
    pubs = dict(zip(keys, oracle.stdout.splitlines()))
    assert len(pubs) == len(keys) and all(len(v) == 130 and v.startswith('04') for v in pubs.values())
    with tempfile.TemporaryDirectory(prefix='keyhunt-c11-probes-') as folder:
        folder = Path(folder)
        targets, table = folder/'targets.txt', folder/'babies.khb'
        run([binary, 'bsgs-table', 'build', '--m', '1024', '--output', table])
        for case in cases:
            mode = case['argv'][1]
            interval = case['argv'][case['argv'].index('-r')+1]
            begin, end = (int(value, 16) for value in interval.split(':'))
            expected = [int(value, 16) for value in case['expected']]
            data = '\n'.join(pubs[k][2:66] if mode == 'xpoint' else pubs[k] for k in expected)+'\n'
            assert data == case['targets'], 'pinned oracle disagrees with C06 fixtures'
            targets.write_text(data)
            for kind in (('direct', 'stepped') if mode == 'xpoint' else ('auto', '1', '8')):
                for capacity in (1, 64):
                    words = ['--range', interval, '--targets', str(targets),
                             '--candidate-capacity', str(capacity)]
                    words += (['--kernel', kind, '--batch-size', '1024'] if mode == 'xpoint'
                              else ['--group-size', kind, '--table', str(table),
                                    '--giant-batch', '2048', '--target-batch', '64'])
                    result = inspect_search(binary, mode, words, begin, end, expected, capacity)
                    report['regressions'].append({'name': case['name'], 'kind': kind,
                                                  'capacity': capacity, **result})
            print(f"regression {case['name']}: passed", flush=True)
        targets.write_text(pubs[1][2:66]+'\n'+pubs[2][2:66]+'\n')
        for sample in range(3):
            for capacity in ((1, 2) if sample % 2 == 0 else (2, 1)):
                words = ['--range', '1:1001', '--targets', str(targets), '--kernel', 'stepped',
                         '--batch-size', '4096', '--candidate-capacity', str(capacity)]
                result = inspect_search(binary, 'xpoint', words, 1, 4097, [1, 2], capacity)
                report['overflow_performance'].append({'sample': sample, 'capacity': capacity, **result})
        print('overflow performance: complete', flush=True)
        targets.write_text('00'*32+'\n')
        begin, count = 1 << 200, 4*1048576
        for sample in range(3):
            for capacity in ((1024, 1048576) if sample % 2 == 0 else (1048576, 1024)):
                words = ['--range', f'{begin:x}:{begin+count:x}', '--targets', str(targets),
                         '--kernel', 'stepped', '--batch-size', '1048576',
                         '--candidate-capacity', str(capacity)]
                result = inspect_search(binary, 'xpoint', words, begin, begin+count, [], capacity)
                report['capacity_performance'].append({'sample': sample, 'capacity': capacity, **result})
        print('capacity performance: complete', flush=True)
    if args.benchmark:
        report['benchmark_sha256'] = sha(args.benchmark)
        report['variant_benchmark_sha256'] = sha(args.variant_benchmark)
        # Five outer pairs alternate process order; each benchmark internally
        # warms its executors and checks five samples per kernel/workload.
        for sample in range(5):
            order = ('baseline', 'variant') if sample % 2 == 0 else ('variant', 'baseline')
            for kind in order:
                path = args.benchmark if kind == 'baseline' else args.variant_benchmark
                result = json.loads(run([path.resolve(), '0', '1048576']))
                report['launch_bound_benchmarks'].append({'sample': sample, 'kind': kind, 'result': result})
            print(f'launch-bound pair {sample+1}/5: complete', flush=True)
    report['passed'] = True
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(report["regressions"])} regression runs passed; {args.report}', flush=True)


if __name__ == '__main__':
    main()
