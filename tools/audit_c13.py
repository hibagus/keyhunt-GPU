#!/usr/bin/env python3
"""Audit durable regressions and MI300X timings using synthetic public targets.

Run after other GPU tests finish; honors caller GPU visibility. State and tables
use a temporary directory. Measures real CLI processes without kernel changes.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(binary, words, timeout=180):
    argv = [str(binary), *map(str, words)]
    start = time.perf_counter_ns()
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    elapsed = (time.perf_counter_ns()-start)/1e6
    if result.returncode or result.stderr:
        raise RuntimeError(f'{argv}: {result.returncode}: {result.stderr}')
    return [json.loads(line) for line in result.stdout.splitlines()], elapsed


def setup(binary, state, mode, begin, width, inputs):
    def local(family, action, *words):
        return invoke(binary, [family, action, '--state-dir', state, *words])[0][0]
    project = local('state', 'project-create', '--name', 'Synthetic C13 audit')['project']
    job = local('checkpoint', 'create', '--project', project, '--mode', mode,
                '--range', f'{begin:x}:{begin+width:x}', '--block-width', f'{width:x}', *inputs)['job']
    grant = local('state', 'claim', '--project', project, '--job', job,
                  '--owner', 'audit', '--request', 'claim')['assignments'][0]['grant']
    return project, job, grant


def check_durable(binary, state, project, job, rows, width, expected):
    summary = rows[-1]
    assert summary['type'] == 'summary' and summary['complete'] and summary['durable_coverage']
    assert int(summary['computed_scalars'], 16) == width
    assert int(summary['resumed_scalars'], 16) == 0
    invoke(binary, ['state', 'check', '--state-dir', state])
    records, _ = invoke(binary, ['checkpoint', 'results', '--state-dir', state,
                                '--project', project, '--job', job, '--limit', '1000'])
    assert sorted(int(r['scalar'], 16) for r in records[0]['results']) == sorted(expected)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--long-batches', type=int, default=4096)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10 or not 65 <= args.long_batches <= 16384:
        parser.error('repeats must be 1..10 and long-batches 65..16384')
    build = args.build.resolve(strict=True)
    binary = build/'keyhunt'
    report = {'recorded_utc': datetime.now(timezone.utc).isoformat(),
              'binary_sha256': digest(binary), 'oracle_sha256': digest(build/'secp256k1_oracle'),
              'inventory': invoke(binary, ['devices', '--backend', 'hip'])[0][0],
              'visibility': {k: os.environ.get(k) for k in
                             ('HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES')},
              'warm_benchmarks': [], 'regressions': [], 'process_samples': [],
              'timing_scope': 'CLI process wall includes startup, input/table preparation, GPU work, output and cleanup. Job creation and post-run integrity checks excluded.'}

    def save():
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2)+'\n')

    with tempfile.TemporaryDirectory(prefix='keyhunt-c13-probes-') as temporary:
        folder = Path(temporary)
        table = folder/'babies.khb'
        invoke(binary, ['bsgs-table', 'build', '--m', '65537', '--output', table])
        # Existing benchmark programs verify matches and counts before accepting
        # timings. Each process warms the owners and retains five samples.
        for repeat in range(args.repeats):
            for mode in (('xpoint', 'bsgs') if repeat % 2 == 0 else ('bsgs', 'xpoint')):
                bench = build/('hip_xpoint_benchmark' if mode == 'xpoint' else 'hip_bsgs_search_benchmark')
                words = ['0', '1048576'] if mode == 'xpoint' else ['0', '65537', '32768']
                data, _ = invoke(bench, words)
                report['warm_benchmarks'].append({'mode': mode, 'repeat': repeat, 'binary_sha256': digest(bench), 'data': data[0]})
            print(f'warm benchmark pair {repeat+1}/{args.repeats}: passed', flush=True)
        old = json.loads((ROOT/'docs/audits/C06_AUDIT_PROBES.json').read_text())
        for case in old['current_search']['cases']:
            mode = case['argv'][1]
            interval = case['argv'][case['argv'].index('-r')+1]
            begin, end = (int(v, 16) for v in interval.split(':'))
            target = folder/'regression-targets'
            target.write_text(case['targets'])
            inputs = ['--targets', target] + (['--table', table] if mode == 'bsgs' else [])
            geometry = ['--batch-size', '1024'] if mode == 'xpoint' else ['--giant-batch', '32', '--target-batch', '64', '--group-size', '8']
            state = folder/case['name']
            project, job, grant = setup(binary, state, mode, begin, end-begin, inputs)
            words = ['checkpoint', 'run', '--state-dir', state, '--backend', 'hip', '--device', '0',
                     '--grant', grant, '--candidate-capacity', '1', *inputs, *geometry]
            rows, elapsed = invoke(binary, words)
            summary = check_durable(binary, state, project, job, rows, end-begin,
                                    [int(v, 16) for v in case['expected']])
            replay, _ = invoke(binary, words)
            assert replay[-1]['batches'] == 0 and int(replay[-1]['resumed_scalars'], 16) == end-begin
            report['regressions'].append({'name': case['name'], 'passed': True, 'expected': case['expected'],
                                          'summary': summary, 'finished_retry': replay[-1], 'process_wall_ms': elapsed})
            print(f'durable regression {case["name"]}: passed', flush=True)
        # Exact known scalar outside both measured ranges; its X-only partner
        # n-k is also outside. No search-under-test output defines the target.
        public = subprocess.run([str(build/'secp256k1_oracle')], input=f'pub {1<<80:064x}\n',
                                text=True, capture_output=True, check=True, timeout=30).stdout.strip()
        assert len(public) == 130 and public.startswith('04')
        files = {'xpoint': folder/'x.txt', 'bsgs': folder/'b.txt'}
        files['xpoint'].write_text(public[2:66]+'\n')
        files['bsgs'].write_text(public+'\n')
        variants = ('volatile', 'timed', 'every-batch')
        begin = 1 << 200
        # One short warm-up for each path, then rotating process order and two
        # workload sizes. Difference in elapsed time estimates incremental cost;
        # it is not a direct in-process steady-state measurement.
        for repeat in range(-1, args.repeats):
            order = variants[repeat % 3:] + variants[:repeat % 3]
            for mode in ('xpoint', 'bsgs'):
                units = 1048576 if mode == 'xpoint' else 65537*32768
                inputs = ['--targets', files[mode]] + (['--table', table] if mode == 'bsgs' else [])
                geometry = ['--batch-size', '1048576'] if mode == 'xpoint' else ['--giant-batch', '32768', '--target-batch', '1']
                sizes = (64,) if repeat < 0 else ((64, args.long_batches) if repeat % 2 == 0 else (args.long_batches, 64))
                for batches in sizes:
                    width = units*batches
                    for variant in order:
                        if variant == 'volatile':
                            rows, wall = invoke(binary, [mode, '--backend', 'hip', '--device', '0',
                                                        '--range', f'{begin:x}:{begin+width:x}', *inputs, *geometry])
                            summary = rows[-1]
                            field = 'verified_steps' if mode == 'xpoint' else 'verified_scalars'
                            assert summary['complete'] and int(summary[field], 16) == width
                            assert int(summary['matches'], 16) == 0 and summary['launch_count'] == batches
                        else:
                            state = folder/f'{repeat}-{mode}-{batches}-{variant}'
                            project, job, grant = setup(binary, state, mode, begin, width, inputs)
                            rows, wall = invoke(binary, ['checkpoint', 'run', '--backend', 'hip', '--device', '0',
                                                       '--state-dir', state, '--grant', grant, *inputs, *geometry,
                                                       '--checkpoint-seconds', '10' if variant == 'timed' else '0'])
                            summary = check_durable(binary, state, project, job, rows, width, [])
                            assert summary['batches'] == batches and summary['match_observations'] == 0
                            if variant == 'every-batch':
                                assert summary['checkpoints'] == batches
                        if repeat >= 0:
                            report['process_samples'].append({'mode': mode, 'variant': variant, 'repeat': repeat,
                                'batches': batches, 'scalar_width': width, 'process_wall_ms': wall, 'summary': summary})
                        save()
                    print(f'process timings round {repeat}: {mode}, {batches} batches complete', flush=True)
    report['incremental_estimates'] = []
    for mode in ('xpoint', 'bsgs'):
        for variant in ('volatile', 'timed', 'every-batch'):
            rates = []
            for repeat in range(args.repeats):
                pair = sorted((r for r in report['process_samples'] if r['mode'] == mode and
                               r['variant'] == variant and r['repeat'] == repeat), key=lambda r: r['batches'])
                short, long = pair
                assert long['process_wall_ms'] > short['process_wall_ms']
                rates.append((long['scalar_width']-short['scalar_width'])*1000 /
                             (long['process_wall_ms']-short['process_wall_ms']))
            report['incremental_estimates'].append({'mode': mode, 'variant': variant,
                'scalar_range_per_second': rates, 'median': statistics.median(rates)})
    report['passed'] = True
    save()
    print(f'audit complete: {args.report}', flush=True)


if __name__ == '__main__':
    main()
