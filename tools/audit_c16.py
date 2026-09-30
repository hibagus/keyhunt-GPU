#!/usr/bin/env python3
"""Independently check C16 evidence and reproduce mixed BSGS group reporting.

The optional live probe uses the frozen checkout's unmodified benchmark harness,
synthetic targets and a caller-selected visible GPU. Run after other GPU tests.
"""
import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tarfile
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--build', type=Path, help='also run the real HIP mixed-group probe')
    args = parser.parse_args()
    source = args.source.resolve()
    baseline = source / 'docs/baselines'
    report = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), source=str(source),
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
                  published_evidence={}, benchmarks={}, traces={})
    validation = json.loads((baseline / 'C16_VALIDATION.json').read_text())
    for name, info in validation['evidence'].items():
        data = (baseline / name).read_bytes()
        assert len(data) == info['bytes'] and digest(data) == info['sha256'], name
        report['published_evidence'][name] = dict(info, verified=True)
    with tarfile.open(baseline / 'C16_RAW_LOGS.tar.gz') as archive:
        artifacts = {m.name: archive.extractfile(m).read() for m in archive if m.isfile()}
    sys.path.insert(0, str(source / 'tests/oracle'))
    from model import multiply, encode

    def rows(prefix, command):
        return [json.loads(line) for line in artifacts[prefix + '/' + command['stdout']].decode().splitlines()]

    for prefix, name in [('matrix', 'C16_MEASUREMENTS.json'), ('overflow', 'C16_OVERFLOW.json'),
                         ('cadence', 'C16_CADENCE.json')]:
        saved = json.loads((baseline / name).read_text())
        assert saved['passed'] and not saved['failures']
        command_by_state = defaultdict(dict)
        checked_commands = 0
        for command in saved['commands']:
            assert command['exit_code'] == 0
            for stream in ('stdout', 'stderr'):
                assert digest(artifacts[prefix + '/' + command[stream]]) == command[stream + '_sha256']
            argv = command['argv']
            if '--state-dir' in argv:
                state = argv[argv.index('--state-dir') + 1]
                command_by_state[state][tuple(argv[1:3])] = command
            checked_commands += 1
        measured = warmups = statistic_fields = 0
        for case in saved['cases']:
            begin, end = int(case['begin'], 16), int(case['end_exclusive'], 16)
            width = end - begin
            useful = width if case['mode'] == 'xpoint' else ((width + case['m'] - 1) // case['m']) * case['target_count']
            seeds = [begin, begin + width // 2, end - 1] if case['workload'] == 'boundary-3' else list(range(1, case['target_count'] + 1))
            public = [encode(multiply(k)) for k in seeds]
            values = [p[2:66] if case['mode'] == 'xpoint' else p for p in public]
            assert sorted(values) == case['target_values']
            expected = {(k, v) for k, v in zip(seeds, values) if begin <= k < end}
            assert expected == {(int(k, 16), v) for k, v in case['expected_matches']}
            for sample in case['samples']:
                command = sample['command']
                output = rows(prefix, command)
                summary, metrics = output[-1], sample['metrics']
                assert summary == sample['summary'] and summary['complete']
                assert int(metrics['scalar_coverage']) == width and int(metrics['useful_device_steps']) == useful
                assert int(summary['device_steps'], 16) == int(metrics['attempted_device_steps'])
                assert int(metrics['overflow_device_steps']) == int(metrics['attempted_device_steps']) - useful >= 0
                for key, value in [('process_scalars_per_s', width * 1000 / command['process_wall_ms']),
                                   ('process_useful_steps_per_s', useful * 1000 / command['process_wall_ms']),
                                   ('executor_scalars_per_s', width * 1000 / metrics['executor_wall_ms'])]:
                    assert math.isclose(metrics[key], value, rel_tol=1e-12)
                if sample['variant'] == 'volatile':
                    assert output[0]['uuid'] == saved['metadata']['selected_device']['uuid']
                    coverage = [r for r in output if r['type'] == ('batch' if case['mode'] == 'xpoint' else 'tile') and not r.get('overflow', False)]
                    cursor = begin
                    for receipt in coverage:
                        assert int(receipt['begin'], 16) == cursor
                        cursor = int(receipt['end_exclusive'], 16)
                    assert cursor == end
                    key = 'x' if case['mode'] == 'xpoint' else 'public_key'
                    matches = [(int(m['scalar'], 16), m[key]) for r in output if r['type'] == 'batch' for m in r['matches']]
                else:
                    state = command['argv'][command['argv'].index('--state-dir') + 1]
                    post = command_by_state[state]
                    block = rows(prefix, post[('state', 'block')])[0]
                    assert block['state'] == 'finished' and not block['remaining']
                    assert [(int(i['begin'], 16), int(i['end_exclusive'], 16)) for i in block['covered']] == [(begin, end)]
                    results = rows(prefix, post[('checkpoint', 'results')])[0]['results']
                    matches = [(int(m['scalar'], 16), m['target_bytes']) for m in results]
                    assert summary['checkpoint_seconds'] == (10 if sample['variant'] == 'timed' else 0)
                assert len(matches) == len(set(matches)) and set(matches) == expected
                if sample['warmup']:
                    warmups += 1
                else:
                    measured += 1
            for variant, fields in case['statistics'].items():
                samples = [s for s in case['samples'] if s['variant'] == variant and not s['warmup']]
                for key, expected_stats in fields.items():
                    data = [s['metrics'][key] for s in samples]
                    middle = statistics.median(data)
                    actual = dict(count=len(data), median=middle, min=min(data), max=max(data),
                                  spread=max(data) - min(data), mad=statistics.median(abs(v - middle) for v in data))
                    assert actual == expected_stats, (case['id'], variant, key)
                    statistic_fields += 1
        report['benchmarks'][prefix] = dict(commands=checked_commands, measured=measured, warmups=warmups,
                                             statistic_fields=statistic_fields, passed=True)
    profiles = json.loads((baseline / 'C16_PROFILES.json').read_text())
    for name in ('xpoint', 'bsgs', 'counter'):
        profile = profiles[name]
        records = []
        for item in profile['artifacts']:
            data = artifacts['profile-' + name + '/' + item['path']]
            assert len(data) == item['bytes'] and digest(data) == item['sha256']
            if item['path'].endswith('kernel_trace.csv'):
                records.extend(csv.DictReader(io.StringIO(data.decode())))
        assert dict(Counter(r['Kernel_Name'] for r in records)) == profile['trace_kernel_counts']
        totals, counts = defaultdict(float), Counter()
        for row in records:
            kind = 'search' if 'keyhunt::gpu::' in row['Kernel_Name'] else 'clear' if 'fillBuffer' in row['Kernel_Name'] else 'copy'
            counts[kind] += 1
            totals[kind] += (int(row['End_Timestamp']) - int(row['Start_Timestamp'])) / 1e6
        assert counts['search'] == profile['trace_search_launches'] == 512
        report['traces'][name] = dict(counts=counts, duration_ms=totals,
            internal_fraction_of_dispatch_time=(totals['clear'] + totals['copy']) / sum(totals.values()),
            scope='Instrumented dispatch durations, not unprofiled application speedup estimates.')

    if args.build:
        build = args.build.resolve()
        base = Path(tempfile.mkdtemp(prefix='kh-c16-mixed-'))
        command = [sys.executable, str(source / 'tools/benchmark_gpu.py'), '--build-dir', str(build),
                   '--output-dir', str(base / 'benchmark'), '--device', '0', '--repeats', '5',
                   '--modes', 'bsgs', '--workloads', 'no-match-32', '--variants', 'volatile', 'timed',
                   '--m', '17', '--batches', '2', '--giant-batch', '8192', '--target-batch', '31']
        completed = subprocess.run(command, capture_output=True, text=True, timeout=180)
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
        live = json.loads((base / 'benchmark/report.json').read_text())
        case = live['cases'][0]
        samples = []
        for sample in case['samples']:
            out = base / 'benchmark' / sample['command']['stdout']
            records = [json.loads(line) for line in out.read_text().splitlines()]
            samples.append(dict(round=sample['round'], warmup=sample['warmup'], variant=sample['variant'],
                                summary=sample['summary'], metrics=sample['metrics'],
                                volatile_dispatches=[{k: r[k] for k in ('first_target', 'target_count', 'group_size', 'device_steps')}
                                                     for r in records if r['type'] == 'batch']))
        report['mixed_group_probe'] = dict(command=command, output=completed.stdout, artifact_directory=str(base),
            benchmark_sha256=digest((base / 'benchmark/report.json').read_bytes()),
            binary_sha256=live['metadata']['binary_sha256'], device=live['metadata']['selected_device'],
            case={k: v for k, v in case.items() if k not in ('samples', 'statistics')}, samples=samples)
        # Trace a fresh durable grant too: replaying an already finished command
        # would legitimately do zero GPU work and cannot prove mixed dispatch.
        binary = build / 'keyhunt'
        state = base / 'trace-state'
        def local(*words):
            result = subprocess.run([str(binary), *map(str, words)], capture_output=True, text=True, timeout=90)
            assert result.returncode == 0, result.stderr
            return [json.loads(line) for line in result.stdout.splitlines()]
        project = local('state', 'project-create', '--state-dir', state, '--name', 'C16 mixed-group audit')[0]['project']
        width = int(case['end_exclusive'], 16) - int(case['begin'], 16)
        table = base / 'benchmark/babies.khb'
        created = local('checkpoint', 'create', '--state-dir', state, '--project', project,
                        '--mode', 'bsgs', '--range', case['begin'] + ':' + case['end_exclusive'],
                        '--block-width', f'{width:x}', '--table', table, '--targets', case['targets'])[0]
        grant = local('state', 'claim', '--state-dir', state, '--project', project, '--job', created['job'],
                      '--owner', 'audit', '--request', 'trace')[0]['assignments'][0]['grant']
        version = subprocess.check_output(['rocprofv3', '--version'], text=True)
        help_text = subprocess.check_output(['rocprofv3', '--help'], text=True)
        assert '--kernel-trace' in help_text and '--output-directory' in help_text
        trace_command = ['rocprofv3', '--kernel-trace', '--output-format', 'csv', '--output-directory', str(base / 'trace'),
                         '--output-file', 'durable', '--', str(binary), 'checkpoint', 'run', '--state-dir', str(state),
                         '--backend', 'hip', '--device', '0', '--grant', grant, '--targets', case['targets'],
                         '--table', str(table), *case['geometry']]
        traced = subprocess.run(trace_command, capture_output=True, text=True, timeout=120)
        assert traced.returncode == 0, traced.stderr
        (base / 'trace.stdout').write_text(traced.stdout)
        (base / 'trace.stderr').write_text(traced.stderr)
        summary = [json.loads(line) for line in traced.stdout.splitlines() if line.startswith('{"type":')][-1]
        local('state', 'check', '--state-dir', state)
        records = []
        trace_files = list((base / 'trace').rglob('*kernel_trace.csv'))
        assert trace_files
        for file in trace_files:
            with file.open() as stream:
                records.extend(csv.DictReader(stream))
        search = [r for r in records if 'keyhunt::gpu::bsgs_search' in r['Kernel_Name']]
        assert len(search) == summary['batches'] == 4
        report['mixed_group_probe']['durable_trace'] = dict(command=trace_command, profiler_version=version,
            summary=summary, kernel_counts=dict(Counter(r['Kernel_Name'] for r in search)),
            dispatches=search, trace_sha256={str(p.relative_to(base)): digest(p.read_bytes()) for p in trace_files},
            note='Profiling timing is excluded from benchmark statistics.')
    report['completed'] = True
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['benchmarks']))
    print('Audit evidence saved to', args.report)


if __name__ == '__main__':
    main()
