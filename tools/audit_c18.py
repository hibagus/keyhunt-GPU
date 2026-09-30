#!/usr/bin/env python3
"""Compare frozen native CUDA builds using checked warm search benchmarks.

Run after correctness tests. This audit driver reuses the repository's bounded
benchmarks and process recorder, while retaining both builds' flags and hashes.
It does not rebuild, modify sources, or change GPU configuration.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--portable', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if not 5 <= args.repeats <= 20 or args.device < 0:
        parser.error('use 5–20 process pairs and a nonnegative visible device')
    source, output = args.source.resolve(), args.output_dir.resolve()
    if output.exists() or output == source or source in output.parents:
        parser.error('output-dir must be new and outside the frozen checkout')
    sys.path.insert(0, str(source/'tools'))
    from benchmark_gpu import Runner, metadata, save, sha, smi
    from benchmark_gpu_pair import warm_samples, paired_statistics
    output.mkdir(mode=0o700, parents=True)
    runner = Runner(output, 120)
    builds = dict(baseline=args.portable.resolve(), candidate=args.build.resolve())
    names = ('keyhunt', 'secp256k1_oracle', 'cuda_xpoint_benchmark', 'cuda_bsgs_search_benchmark')
    fingerprints = {name: {binary: sha(build/binary) for binary in names} for name, build in builds.items()}
    switches = ('KEYHUNT_CUDA_PORTABLE_CARRY', 'KEYHUNT_CUDA_PORTABLE_INVERSE', 'KEYHUNT_CUDA_PORTABLE_MIXED')
    for name, build in builds.items():
        cache = (build/'CMakeCache.txt').read_text()
        assert 'KEYHUNT_ENABLE_CUDA:BOOL=ON' in cache
        flags = next(line for line in cache.splitlines() if line.startswith('CMAKE_CUDA_FLAGS:STRING='))
        assert all((switch in flags) == (name == 'baseline') for switch in switches)
    report = dict(schema_version=1, recorded_utc=datetime.now(timezone.utc).isoformat(),
                  label='Independent C18 native CUDA arithmetic comparison', passed=False,
                  snapshots={}, pairs=[], warm_records=[], cli_records=[], commands=runner.commands,
                  failures=[], options=dict(suite='warm', backend='cuda', kernel='stepped',
                  batch_size=1048576, m=65537, giant_batch=32768, candidate_capacity=1024,
                  repeats=args.repeats, device=args.device, output_dir=str(output),
                  portable_definitions=list(switches)),
                  benchmark_sources={name: sha(source/name) for name in
                  ('tests/gpu/xpoint_benchmark.cpp', 'tests/gpu/bsgs_search_benchmark.cpp')},
                  visibility={k: os.environ.get(k) for k in ('CUDA_VISIBLE_DEVICES', 'HIP_VISIBLE_DEVICES')},
                  runtime_controls={k: os.environ.get(k) for k in ('CUDA_MODULE_LOADING',
                  'CUDA_FORCE_PTX_JIT', 'CUDA_DISABLE_PTX_JIT', 'CUDA_CACHE_DISABLE', 'CUDA_LAUNCH_BLOCKING')},
                  cpu_affinity=sorted(os.sched_getaffinity(0)))
    try:
        for name, build in builds.items():
            rows, _ = runner.run([build/'keyhunt', 'devices', '--backend', 'cuda'])
            inventory = rows[0]
            device = next(d for d in inventory['devices'] if d['ordinal'] == args.device)
            report['snapshots'][name] = metadata(build, build/'keyhunt', build/'secp256k1_oracle',
                output, device, inventory, 'Unreserved H200 host; no audit tests/builds during timing.')
            report['snapshots'][name]['binaries'] = fingerprints[name]
            if 'device' in report:
                assert report['device']['uuid'] == device['uuid']
            report['device'] = device
        report['smi_before'] = smi(device)
        for repeat in range(-1, args.repeats):
            pair = dict(round=repeat, warmup=repeat < 0, baseline={}, candidate={})
            report['pairs'].append(pair)
            order = ('baseline', 'candidate') if repeat % 2 else ('candidate', 'baseline')
            for mode in ('xpoint', 'bsgs'):
                for name in order:
                    build = builds[name]
                    command = ([build/'cuda_xpoint_benchmark', args.device, 1048576, 'stepped', 1024]
                               if mode == 'xpoint' else
                               [build/'cuda_bsgs_search_benchmark', args.device, 65537, 32768, 1024])
                    rows, invocation = runner.run(command)
                    assert len(rows) == 1
                    record = rows[0]
                    report['warm_records'].append(dict(round=repeat, variant=name, mode=mode,
                                                        record=record, command=invocation))
                    pair[name].update(warm_samples(record, mode, 1048576, 65537, 32768))
            assert pair['baseline'].keys() == pair['candidate'].keys()
            save(output/'report.json', report)
            print(f'CUDA audit pair {repeat} checked', flush=True)
        report['statistics'] = paired_statistics([p for p in report['pairs'] if not p['warmup']])
        for name, build in builds.items():
            assert fingerprints[name] == {binary: sha(build/binary) for binary in names}
        report['smi_after'] = smi(device)
        report['passed'] = True
    except Exception as error:
        report['failures'].append(f'{type(error).__name__}: {error}')
        raise
    finally:
        save(output/'report.json', report)
    print('CUDA audit evidence:', output/'report.json')


if __name__ == '__main__':
    main()
