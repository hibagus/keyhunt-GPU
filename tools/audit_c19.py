#!/usr/bin/env python3
"""Independently recompute C17/C19 evidence and inspect retained C18 results.

No GPU work is launched. Optional --pair/--matrix arguments check fresh reports
and their raw logs too. The source must be a frozen C19 checkout. Assertions are
intentional acceptance gates; run without Python's -O option.
"""
import argparse
from collections import defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tarfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def distribution(values):
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    middle = statistics.median(values)
    return dict(count=len(values), median=middle, min=min(values), max=max(values),
                spread=max(values)-min(values), mad=statistics.median(abs(v-middle) for v in values))


def close(a, b):
    assert math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-6), (a, b)


def command_logs(saved, read):
    states = defaultdict(dict)
    for c in saved['commands']:
        assert c['exit_code'] == 0, c
        for stream in ('stdout', 'stderr'):
            assert sha(read(c[stream])) == c[stream+'_sha256'], c[stream]
        argv = c['argv']
        if '--state-dir' in argv:
            states[argv[argv.index('--state-dir')+1]][tuple(argv[1:3])] = c
    return states


def records(read, command):
    return [json.loads(line) for line in read(command['stdout']).decode().splitlines()]


def case_work(case):
    begin, end = int(case['begin'], 16), int(case['end_exclusive'], 16)
    width = end-begin
    steps = width if case['mode'] == 'xpoint' else ((width+case['m']-1)//case['m'])*case['target_count']
    return begin, end, width, steps


def check_case(case, multiply, encode):
    begin, end, width, _ = case_work(case)
    seeds = (list(range(begin, begin+4)) if case['workload'] == 'dense-prefix' else
             [begin, begin+width//2, end-1] if case['workload'] == 'boundary-3' else
             list(range(1, case['target_count']+1)))
    public = [encode(multiply(k)) for k in seeds]
    values = [p[2:66] if case['mode'] == 'xpoint' else p for p in public]
    assert sorted(values) == case['target_values']
    assert {(k, v) for k, v in zip(seeds, values) if begin <= k < end} == {
        (int(k, 16), v) for k, v in case['expected_matches']}


def check_sample(case, sample, read=None, states=None):
    begin, end, width, useful = case_work(case)
    summary, metrics = sample['summary'], sample['metrics']
    assert summary['complete']
    assert int(metrics['scalar_coverage']) == width and int(metrics['useful_device_steps']) == useful
    attempted = int(summary['device_steps'], 16)
    assert attempted == int(metrics['attempted_device_steps']) >= useful
    assert attempted-useful == int(metrics['overflow_device_steps'])
    for key, value in [('process_scalars_per_s', width*1000/sample['command']['process_wall_ms']),
                       ('executor_scalars_per_s', width*1000/metrics['executor_wall_ms']),
                       ('process_useful_steps_per_s', useful*1000/sample['command']['process_wall_ms']),
                       ('kernel_attempted_steps_per_s', attempted*1000/metrics['kernel_ms'])]:
        close(metrics[key], value)
    if sample['variant'] != 'volatile' and case['mode'] == 'bsgs' and summary['metrics_version'] == 2:
        groups = summary['bsgs_groups']
        assert metrics['actual_groups'] == [g['group_size'] for g in groups]
        assert metrics['actual_groups'] == sorted(set(metrics['actual_groups']))
        assert all(g['group_size'] in (1, 8) for g in groups)
        for k in ('batches', 'overflow_replays'):
            assert sum(g[k] for g in groups) == summary[k]
        for k in ('device_steps', 'verified_device_steps'):
            assert sum(int(g[k], 16) for g in groups) == int(summary[k], 16)
        close(sum(g['kernel_ms'] for g in groups), summary['kernel_ms'])
    if read is None:
        return
    rows = records(read, sample['command'])
    assert rows[-1] == summary
    expected = {(int(k, 16), v) for k, v in case['expected_matches']}
    if sample['variant'] == 'volatile':
        cursor = begin
        for row in rows:
            if row['type'] == ('batch' if case['mode'] == 'xpoint' else 'tile') and not row.get('overflow', False):
                assert int(row['begin'], 16) == cursor
                cursor = int(row['end_exclusive'], 16)
        assert cursor == end
        key = 'x' if case['mode'] == 'xpoint' else 'public_key'
        matches = [(int(m['scalar'], 16), m[key]) for r in rows if r['type'] == 'batch' for m in r['matches']]
        assert sum(r['device_steps'] for r in rows if r['type'] == 'batch') == attempted
    else:
        argv = sample['command']['argv']
        state = states[argv[argv.index('--state-dir')+1]]
        block = records(read, state[('state', 'block')])[0]
        assert block['state'] == 'finished' and not block['remaining']
        assert [(int(r['begin'], 16), int(r['end_exclusive'], 16)) for r in block['covered']] == [(begin, end)]
        results = records(read, state[('checkpoint', 'results')])[0]['results']
        matches = [(int(m['scalar'], 16), m['target_bytes']) for m in results]
        assert len(rows)-1 == summary['checkpoints']
        close(sum(r['transaction_ms'] for r in rows[:-1]), summary['checkpoint_ms'])
    assert len(matches) == len(set(matches)) and set(matches) == expected


def check_pair(saved, read, multiply, encode):
    assert saved['passed'] and not saved['failures']
    states = command_logs(saved, read)
    reconstructed = {(p['round'], v): {} for p in saved['pairs'] for v in ('baseline', 'candidate')}
    for entry in saved['warm_records']:
        raw = records(read, entry['command'])
        assert raw == [entry['record']]
        record, mode = entry['record'], entry['mode']
        for case in record['workloads']:
            kinds = (saved['options']['kernel'],) if mode == 'xpoint' else (0, 1, 8)
            for kind in kinds:
                samples = [s for s in case['samples'] if s['sample'] >= 0 and
                           (s['kernel'] if mode == 'xpoint' else s['group_size']) == kind]
                assert len(samples) == 5 and {s['sample'] for s in samples} == set(range(5))
                steps = record['count'] if mode == 'xpoint' else record['giants_per_target']*case['targets']
                for s in samples:
                    assert s['device_steps'] == steps and s['matches'] == (case['targets'] if case['name'] == 'boundary_3' else 0)
                key = f'{mode}/{case["name"]}/{kind}'
                reconstructed[entry['round'], entry['variant']][key] = {
                    k: statistics.median(s[k] for s in samples) for k in
                    ('kernel_ms', 'wall_ms', 'download_ms', 'download_bytes', 'device_allocation_bytes')}
    cases = {c['id']: c for c in saved.get('cases', [])}
    for c in cases.values():
        check_case(c, multiply, encode)
    for sample in saved['cli_records']:
        check_sample(cases[sample['case']], sample, read, states)
        key = f'cli/{sample["case"]}/{sample["variant"]}'
        reconstructed[sample['round'], sample['build']][key] = {k: sample['metrics'][k] for k in
            ('process_wall_ms', 'executor_wall_ms', 'kernel_ms', 'download_ms', 'checkpoint_ms', 'download_bytes')}
    for pair in saved['pairs']:
        assert pair['warmup'] == (pair['round'] < 0)
        for variant in ('baseline', 'candidate'):
            assert pair[variant] == reconstructed[pair['round'], variant]
    measured = [p for p in saved['pairs'] if not p['warmup']]
    assert len(measured) == saved['options']['repeats'] >= 5
    count = 0
    for key, metrics in saved['statistics'].items():
        for metric, published in metrics.items():
            a = [p['baseline'][key][metric] for p in measured]
            b = [p['candidate'][key][metric] for p in measured]
            actual = dict(baseline=distribution(a), candidate=distribution(b))
            if all(v > 0 for v in b):
                actual['paired_baseline_over_candidate'] = distribution([x/y for x, y in zip(a, b)])
            assert actual == published, (key, metric)
            count += 1
    return dict(passed=True, commands=len(saved['commands']), measured_pairs=len(measured),
                warmup_pairs=len(saved['pairs'])-len(measured), metric_comparisons=count,
                warm_processes=len(saved['warm_records']), cli_processes=len(saved['cli_records']))


def check_matrix(saved, multiply, encode, read=None):
    assert saved['passed'] and not saved['failures']
    states = command_logs(saved, read) if read else None
    counts = dict(measured=0, warmups=0, distributions=0, raw_logs_checked=read is not None)
    for case in saved['cases']:
        check_case(case, multiply, encode)
        for sample in case['samples']:
            check_sample(case, sample, read, states)
            counts['warmups' if sample['warmup'] else 'measured'] += 1
        for variant, metrics in case['statistics'].items():
            for key, value in metrics.items():
                assert distribution([s['metrics'][key] for s in case['samples']
                                     if s['variant'] == variant and not s['warmup']]) == value
                counts['distributions'] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--pair', type=Path, action='append', default=[])
    parser.add_argument('--matrix', type=Path, action='append', default=[])
    args = parser.parse_args()
    source = args.source.resolve()
    sys.path.insert(0, str(source/'tests/oracle'))
    from model import multiply, encode
    base = source/'docs/baselines'
    report = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), source=str(source),
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
                  paired_reports={}, matrices={}, isa={}, published_files={})
    validation = json.loads((base/'C17_VALIDATION.json').read_text())
    for name, expected in validation['evidence_sha256'].items():
        data = (base/name).read_bytes()
        assert sha(data) == expected, name
        report['published_files'][name] = dict(sha256=expected, bytes=len(data))
    for milestone in ('C17', 'C19'):
        with tarfile.open(base/(milestone+'_RAW_LOGS.tar.gz')) as archive:
            artifacts = {m.name: archive.extractfile(m).read() for m in archive if m.isfile()}
        for path in sorted(base.glob(milestone+'_*.json')):
            saved = json.loads(path.read_text())
            if 'pairs' not in saved:
                continue
            prefix = Path(saved['options']['output_dir']).name
            report['paired_reports'][path.name] = check_pair(saved, lambda n: artifacts[prefix+'/'+n], multiply, encode)
        if milestone == 'C17':
            saved = json.loads((base/'C17_GROUPS.json').read_text())
            prefix = Path(saved['options']['output_dir']).name
            report['matrices']['C17_GROUPS.json'] = check_matrix(saved, multiply, encode, lambda n: artifacts[prefix+'/'+n])
        for name, data in artifacts.items():
            if not name.endswith('/report.json') or '-isa-' not in name:
                continue
            saved = json.loads(data)
            if name.endswith('isa-cpu-negative/report.json'):
                assert not saved['passed'] and not saved['code_objects']
                assert saved['failure'].startswith('no embedded gfx942 code objects')
                report['isa'][name] = {'expected_negative': True}
                continue
            assert saved['passed']
            prefix = name.rsplit('/', 1)[0]
            command_logs(saved, lambda n: artifacts[prefix+'/'+n])
            for item in saved['code_objects']:
                blob = artifacts[prefix+'/'+item['file']]
                assert sha(blob) == item['sha256'] and len(blob) == item['bytes']
            report['isa'][prefix] = dict(commands=len(saved['commands']), code_objects=len(saved['code_objects']))
    validation = json.loads((base/'C19_VALIDATION.json').read_text())
    hashes = {**validation['snapshot']['source_files_sha256'], **validation['test_sources_sha256']}
    report['c19_documentation_hash_changes'] = []
    for name, expected in hashes.items():
        if name.endswith('.md') and sha((source/name).read_bytes()) != expected:
            report['c19_documentation_hash_changes'].append(name)
        else:
            assert sha((source/name).read_bytes()) == expected, name
    report['c19_current_source_hashes'] = len(hashes)-len(report['c19_documentation_hash_changes'])
    # C18 retains JSON results but not the corresponding command-log archive.
    # Check only what is available; never relabel this as an independent GPU run.
    saved = json.loads((base/'C18_DURABILITY.json').read_text())['report']
    report['matrices']['C18_DURABILITY.json'] = check_matrix(saved, multiply, encode)
    saved = json.loads((base/'C18_H200_LARGE_BATCH.json').read_text())
    medians = {}
    for run in saved['runs']:
        record = run['results']
        mode = 'xpoint' if 'count' in record else 'bsgs'
        for case in record['workloads']:
            selector = 'kernel' if mode == 'xpoint' else 'group_size'
            for kind in {s[selector] for s in case['samples']}:
                samples = [s for s in case['samples'] if s[selector] == kind and s['sample'] >= 0]
                assert len(samples) == 5
                expected = record['count'] if mode == 'xpoint' else record['giants_per_target']*case['targets']
                assert all(s['device_steps'] == expected and s['matches'] == (case['targets'] if case['name'] == 'boundary_3' else 0) for s in samples)
                medians[run['stage'], mode, case['name'], kind] = statistics.median(s['kernel_ms'] for s in samples)
    for item in saved['comparison']:
        key = item['mode'], item['workload'], item['variant']
        a, b = medians[('portable', *key)], medians[('optimized', *key)]
        close(a, item['portable_median_ms']); close(b, item['optimized_median_ms']); close(a/b, item['speedup'])
    report['c18_large_batch_comparisons'] = len(saved['comparison'])
    for path in args.pair:
        report['paired_reports'][str(path)] = check_pair(json.loads(path.read_text()), lambda n: (path.parent/n).read_bytes(), multiply, encode)
    for path in args.matrix:
        report['matrices'][str(path)] = check_matrix(json.loads(path.read_text()), multiply, encode, lambda n: (path.parent/n).read_bytes())
    report['passed'] = True
    args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
