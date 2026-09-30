#!/usr/bin/env python3
"""Freeze build artifacts and run alternating C17 baseline/candidate trials."""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

from benchmark_gpu import ROOT, Runner, execute, make_case, metadata, save, sha, smi
sys.path.insert(0, str(ROOT / 'benchmarks'))
from gpu_metrics import distribution, require

BINARIES = ('keyhunt', 'secp256k1_oracle', 'hip_xpoint_benchmark', 'hip_bsgs_search_benchmark')
BENCH_SOURCES = ('tests/gpu/xpoint_benchmark.cpp', 'tests/gpu/bsgs_search_benchmark.cpp')


def snapshot(build, output):
    """Call after building. Frozen executables keep later edits out of the baseline."""
    inventory = json.loads(subprocess.check_output([build/'keyhunt', 'devices', '--backend', 'hip'], text=True))
    captured = metadata(build, build/'keyhunt', build/'secp256k1_oracle', output,
                        inventory['devices'][0], inventory, 'Unreserved host; other workload is not controlled.')
    for name in BINARIES:
        shutil.copy2(build/name, output/name)
    captured['binaries'] = {name: sha(output/name) for name in BINARIES}
    captured['benchmark_sources'] = {name: sha(ROOT/name) for name in BENCH_SOURCES}
    save(output/'snapshot.json', captured)


def load_snapshot(path):
    result = json.loads((path/'snapshot.json').read_text())
    require(all(sha(path/name) == digest for name, digest in result['binaries'].items()), 'frozen binary changed')
    return result


def warm_samples(record, mode, count, m, giants, kernel='stepped'):
    """Keep process medians independent: five samples in one process are not five pairs."""
    require(len(record['workloads']) == 3, 'warm benchmark workload set differs')
    expected_width = count if mode == 'xpoint' else m*giants
    require((record['count'] if mode == 'xpoint' else record['scalar_width']) == expected_width, 'warm width mismatch')
    if mode == 'bsgs':
        require(record['m'] == m and record['giants_per_target'] == giants, 'warm BSGS geometry mismatch')
    result = {}
    for case in record['workloads']:
        kinds = (kernel,) if mode == 'xpoint' else (0, 1, 8)
        for kind in kinds:
            rows = [s for s in case['samples'] if s['sample'] >= 0 and
                    (s['kernel'] if mode == 'xpoint' else s['group_size']) == kind]
            require(len(rows) == 5 and {s['sample'] for s in rows} == set(range(5)), 'warm sample set differs')
            steps = count if mode == 'xpoint' else giants*case['targets']
            for sample in rows:
                require(sample['device_steps'] == steps and sample['matches'] == (case['targets'] if case['name'] == 'boundary_3' else 0),
                        'warm useful work/matches differ')
                require(all(math.isfinite(sample[k]) and sample[k] > 0 for k in ('kernel_ms', 'wall_ms')), 'invalid warm timing')
            key = f'{mode}/{case["name"]}/{kind}'
            result[key] = {k: statistics.median(s[k] for s in rows) for k in
                           ('kernel_ms', 'wall_ms', 'download_ms', 'download_bytes', 'device_allocation_bytes')}
    return result


def paired_statistics(pairs):
    result = {}
    require(bool(pairs), 'no paired measurements')
    for key in pairs[0]['baseline']:
        result[key] = {}
        for metric in pairs[0]['baseline'][key]:
            a = [p['baseline'][key][metric] for p in pairs]
            b = [p['candidate'][key][metric] for p in pairs]
            item = {'baseline': distribution(a), 'candidate': distribution(b)}
            if all(v > 0 for v in b):
                item['paired_baseline_over_candidate'] = distribution([x/y for x,y in zip(a,b)])
            result[key][metric] = item
    return result


def compare(args, output):
    roots = {'baseline': args.baseline.resolve(), 'candidate': args.candidate.resolve()}
    frozen = {name: load_snapshot(path) for name, path in roots.items()}
    require(frozen['baseline']['benchmark_sources'] == frozen['candidate']['benchmark_sources'], 'benchmark source differs')
    report = {'schema_version': 1, 'recorded_utc': datetime.now(timezone.utc).isoformat(), 'passed': False,
              'label': args.label, 'snapshots': frozen, 'pairs': [], 'warm_records': [], 'cli_records': [],
              'failures': [], 'options': {k: str(v) if isinstance(v,Path) else v for k,v in vars(args).items()}}
    runner = Runner(output, args.timeout)
    report['commands'] = runner.commands
    try:
        inventories = {name: runner.run([path/'keyhunt','devices','--backend','hip'])[0][0] for name,path in roots.items()}
        devices = {name: next(d for d in inv['devices'] if d['ordinal'] == args.device) for name, inv in inventories.items()}
        for field in ('uuid','architecture','compute_partition','memory_partition','compute_units'):
            require(devices['baseline'][field] == devices['candidate'][field], 'paired device differs')
        report.update(device=devices['baseline'], cpu_affinity=sorted(os.sched_getaffinity(0)),
                      visibility={k:os.environ.get(k) for k in ('HIP_VISIBLE_DEVICES','ROCR_VISIBLE_DEVICES','CUDA_VISIBLE_DEVICES')},
                      smi_before=smi(devices['baseline']))
        cases, table = [], output/'babies.khb'
        if args.suite in ('cli', 'both'):
            table_info = {}
            if 'bsgs' in args.modes:
                table_info = runner.run([roots['baseline']/'keyhunt','bsgs-table','build','--m',args.m,'--output',table])[0][0]
            # Inputs are created once through the independent C16 oracle path and
            # shared unchanged by both executables and all durability variants.
            for mode in args.modes:
                for workload in args.workloads:
                    cases.append(make_case(args, mode, workload, output, roots['baseline']/'secp256k1_oracle', table_info))
            report['cases'] = cases
        for repeat in range(-1,args.repeats):
            pair = {'round':repeat, 'warmup':repeat<0, 'baseline':{}, 'candidate':{}}
            report['pairs'].append(pair)
            order = ('baseline','candidate') if repeat%2 else ('candidate','baseline')
            for mode in args.modes:
                if args.suite in ('warm','both'):
                    for name in order:
                        path = roots[name]
                        words = ([path/'hip_xpoint_benchmark',args.device,args.batch_size,args.kernel,args.candidate_capacity]
                                 if mode == 'xpoint' else [path/'hip_bsgs_search_benchmark',args.device,args.m,args.giant_batch,args.candidate_capacity])
                        rows, command = runner.run(words)
                        require(len(rows) == 1, 'unexpected benchmark output')
                        report['warm_records'].append({'round':repeat,'variant':name,'mode':mode,'record':rows[0],'command':command})
                        pair[name].update(warm_samples(rows[0],mode,args.batch_size,args.m,args.giant_batch,args.kernel))
                for case in (c for c in cases if c['mode'] == mode):
                    for variant in args.variants:
                        for name in order:
                            # execute() allocates a fresh journal per label. Both
                            # paths use identical search inputs and geometry.
                            unique = {**case, 'id': case['id']+'-'+name}
                            sample = execute(args,runner,unique,variant,repeat,roots[name]/'keyhunt',table,devices[name])
                            report['cli_records'].append({'build':name,'case':case['id'],**sample})
                            pair[name][f'cli/{case["id"]}/{variant}'] = {k:sample['metrics'][k] for k in
                                ('process_wall_ms','executor_wall_ms','kernel_ms','download_ms','checkpoint_ms','download_bytes')}
            require(pair['baseline'].keys() == pair['candidate'].keys(), 'paired workload mismatch')
            save(output/'report.json',report)
            print(f'{args.label}: pair {repeat} checked',flush=True)
        measured = [p for p in report['pairs'] if not p['warmup']]
        report['statistics'] = paired_statistics(measured)
        report['smi_after'] = smi(devices['baseline'])
        report['passed'] = True
    except (OSError,ValueError,RuntimeError,KeyError,StopIteration,subprocess.SubprocessError) as error:
        report['failures'].append(f'{type(error).__name__}: {error}')
    finally:
        save(output/'report.json',report)
    print(f'Paired benchmark {"passed" if report["passed"] else "FAILED"}: {output/"report.json"}')
    return 0 if report['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action',required=True)
    capture = commands.add_parser('snapshot',help='freeze a freshly built HIP directory')
    capture.add_argument('--build-dir',type=Path,required=True)
    capture.add_argument('--output-dir',type=Path,required=True)
    trial = commands.add_parser('compare')
    trial.add_argument('--baseline',type=Path,required=True)
    trial.add_argument('--candidate',type=Path,required=True)
    trial.add_argument('--output-dir',type=Path,required=True)
    trial.add_argument('--label',required=True)
    trial.add_argument('--suite',choices=('warm','cli','both'),default='warm')
    trial.add_argument('--modes',nargs='+',choices=('xpoint','bsgs'),default=['xpoint','bsgs'])
    trial.add_argument('--workloads',nargs='+',choices=('no-match-1','boundary-3','no-match-32'),default=['no-match-1','boundary-3','no-match-32'])
    trial.add_argument('--variants',nargs='+',choices=('volatile','timed','every-batch'),default=['volatile','timed'])
    trial.add_argument('--repeats',type=int,default=5)
    trial.add_argument('--device',type=int,default=0)
    trial.add_argument('--batch-size',type=int,default=1048576)
    trial.add_argument('--batches',type=int,default=512)
    trial.add_argument('--m',type=int,default=65537)
    trial.add_argument('--giant-batch',type=int,default=32768)
    trial.add_argument('--target-batch',type=int,default=32)
    trial.add_argument('--candidate-capacity',type=int,default=1024)
    trial.add_argument('--timeout',type=int,default=300)
    trial.add_argument('--kernel',choices=('stepped','direct'),default='stepped')
    trial.set_defaults(group_size='auto')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists() or output == ROOT or ROOT in output.parents:
        parser.error('output-dir must be new and outside checkout')
    if args.action == 'compare':
        if not 5 <= args.repeats <= 100 or not 3 <= args.batch_size <= 1048576 or not 2 <= args.m <= 1048576 or not 1 <= args.giant_batch <= 32768 or not 1 <= args.target_batch <= 64 or not 1 <= args.batches <= 1000000 or not 3 <= args.candidate_capacity <= (65536 if 'bsgs' in args.modes else 1048576) or args.device < 0 or args.timeout <= 0:
            parser.error('invalid bounded geometry, repetition count, or timeout')
        for key in ('modes','workloads','variants'):
            if len(set(getattr(args,key))) != len(getattr(args,key)):
                parser.error('duplicate '+key)
    output.mkdir(parents=True,mode=0o700)
    if args.action == 'snapshot':
        snapshot(args.build_dir.resolve(),output)
        print(f'Frozen HIP artifacts: {output}')
        return 0
    return compare(args,output)


if __name__ == '__main__':
    raise SystemExit(main())
