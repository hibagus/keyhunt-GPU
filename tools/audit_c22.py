#!/usr/bin/env python3
"""Independently check retained C22 provenance and offline CLI evidence.

This reads evidence; it does not run GPU work or authenticate historical logs.
Assertions are intentionally avoided so Python optimization cannot omit checks.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tarfile

GX = '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
GY = '483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(source, *args):
    return subprocess.check_output(['git', '-C', str(source), *args])


def offline_report(report):
    require(report['passed'], 'reported fixture failed')
    require({c['mode'] for c in report['cases']} == {'xpoint', 'bsgs'} and len(report['cases']) == 2,
            'expected exactly two modes')
    summary = []
    for case in report['cases']:
        mode = case['mode']
        require(case['reserved_blocks'] == 2, 'reservation count changed')
        if 'blocks' not in case:
            require(not report.get('hardware', True), 'hardware evidence missing')
            summary.append(dict(mode=mode, hardware=False))
            continue
        width = case.get('width', 512 if mode == 'xpoint' else 32768)
        require(len(case['blocks']) == 2, 'missing local blocks')
        for number, block in enumerate(case['blocks']):
            require(block['state'] == 'finished' and not block['remaining'], 'local block unfinished')
            ranges = [(int(x['begin'], 16), int(x['end_exclusive'], 16)) for x in block['covered']]
            require(ranges == [(1 + number * width, 1 + (number + 1) * width)], 'coverage mismatch')
        expected_target = GX if mode == 'xpoint' else '04' + GX + GY
        for results in [case['local_results']['results'], case['server_results']]:
            require(len(results) == 1, 'unexpected match count')
            row = results[0]
            require(int(row['scalar'], 16) == 1 and int(row['block'], 16) == 0 and row['target'] == 0,
                    'match identity incorrect')
            require(row['target_bytes'] == expected_target, 'match does not bind public generator fixture')
        pending, final = case['local_status'], case['final_status']
        require(pending['outbox_bytes'] > 0 and final['outbox_bytes'] == 0, 'outbox acknowledgment missing')
        for state, activity in [(pending, 'local-complete-awaiting-sync'), (final, 'server-acknowledged')]:
            require(state['transport'] == 'file' and state['sync_due_in'] is None, 'not file-only state')
            require(len(state['queues']) == 2 and all(q['activity'] == activity for q in state['queues']),
                    'incorrect completion/acknowledgment state')
        require(final['pending_request'] is False and final['pending_file_transfer'] is None,
                'acknowledged request still pending')
        events = case['events']
        starts = [e for e in events if e.get('type') == 'grant-start']
        finishes = [e for e in events if e.get('type') == 'grant-finish']
        ready = [e for e in events if e.get('type') == 'ready']
        require(len(starts) == len(finishes) == 2 and len(ready) == 1, 'owner event count mismatch')
        require(ready[0]['self_test']['passed'] and ready[0]['uuid'] == ready[0]['self_test']['uuid'],
                'self-test/UUID mismatch')
        require([e['cold'] for e in finishes] == [True, False], 'executor not reused')
        m = 1 if mode == 'xpoint' else 257
        for index, (start, finish) in enumerate(zip(starts, finishes)):
            g = finish['grant']
            require(start['grant'] == g, 'grant changed during execution')
            require((int(g['begin'], 16), int(g['end_exclusive'], 16), int(g['block'], 16)) ==
                    (1 + index * width, 1 + (index + 1) * width, index), 'grant grid mismatch')
            require(finish['mode'] == mode and finish['complete'] and finish['executor_setups'] == 1,
                    'grant incomplete or executor reconstructed')
            require(finish['m'] == m and finish['target_count'] == 1, 'search geometry mismatch')
            require(int(finish['computed_scalars'], 16) == width and
                    int(finish['device_steps'], 16) == (width + m - 1) // m, 'work counts incorrect')
            require(math.isfinite(finish['kernel_ms']) and finish['kernel_ms'] >= 0 and finish['wall_ns'] > 0,
                    'invalid timing')
        summary.append(dict(mode=mode, hardware=True, blocks=2, scalars=2 * width,
                            device_steps=2 * ((width + m - 1) // m), uuid=ready[0]['uuid'],
                            executor_setups=1, matches=1))
    require(len(report['commands']) > 0, 'missing command trace')
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--report', action='append', type=Path, default=[])
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    root = a.source.resolve()
    baseline = root / 'docs/baselines'
    manifest = json.loads((baseline / 'C22_VALIDATION.json').read_text())
    result = dict(source=git(root, 'rev-parse', 'HEAD').decode().strip(), passed=False,
                  published={}, fresh={})
    require(manifest['passed'] and manifest['schema_version'] == 7, 'wrong acceptance manifest')
    for group in ['source_sha256', 'publication_sha256', 'published_migrations_unchanged']:
        for name, expected in manifest[group].items():
            require(sha((root / name).read_bytes()) == expected, group + ': ' + name)
            if group == 'source_sha256':
                require(sha(git(root, 'show', manifest['source_commit'] + ':' + name)) == expected,
                        'named source commit mismatch: ' + name)
            elif group == 'published_migrations_unchanged':
                require(sha(git(root, 'show', 'f7242a0:' + name)) == expected, 'C21 migration changed: ' + name)
        result['published'][group] = len(manifest[group])
    for name, expected in manifest['artifacts'].items():
        data = (baseline / name).read_bytes()
        require(len(data) == expected['bytes'] and sha(data) == expected['sha256'], 'artifact mismatch: ' + name)
    with tarfile.open(baseline / 'C22_LOGS.tar.gz') as archive:
        files = {x.name: x for x in archive.getmembers() if x.isfile()}
        require(set(files) == set(manifest['log_members']), 'archive file set mismatch')
        for name, expected in manifest['log_members'].items():
            require(sha(archive.extractfile(files[name]).read()) == expected, 'log mismatch: ' + name)
        result['published']['log_members'] = len(files)
    gpu_paths = ['kernels', 'src/backend', 'include/keyhunt/backend', 'src/scheduler',
                 'include/keyhunt/scheduler', 'src/coordinator/device_worker.cpp']
    gpu_files = git(root, 'ls-files', '--', *gpu_paths).decode().splitlines()
    require(len(gpu_files) > 40, 'GPU source selection unexpectedly empty')
    result['published']['gpu_and_dispatch_files_compared'] = len(gpu_files)
    changes = git(root, 'diff', '--name-only', 'f7242a0', 'HEAD', '--', *gpu_paths).decode().splitlines()
    require(not changes and manifest['gpu_backend_changes'] == [], 'GPU backend changed')
    result['published']['gpu_backend_changes'] = changes
    raw = json.loads((baseline / 'C22_OFFLINE.json').read_text())
    require(set(raw) == {'cpu', 'hip', 'cuda'}, 'missing published backend')
    for backend, report in raw.items():
        require(report['binaries'] == manifest['binary_sha256'][backend], 'recorded binary hashes disagree')
        result['published'][backend] = offline_report(report)
    for path in a.report:
        result['fresh'][str(path)] = dict(sha256=sha(path.read_bytes()),
                                        cases=offline_report(json.loads(path.read_text())))
    result['passed'] = True
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS C22 evidence, provenance, exact coverage, results and executor reuse')


if __name__ == '__main__':
    main()
