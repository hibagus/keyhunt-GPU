#!/usr/bin/env python3
"""Recompute the C21 paired BSGS geometry experiment and CPU-oracle edge checks.

The live measurement/edge drivers are retained in C21_AUDIT_RAW_LOGS.tar.gz.
This checker launches no GPU work and imports no production benchmark metrics.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

from audit_c21 import distribution, fleet, module, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--edges', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    oracle = module(args.source/'tests/oracle/model.py', 'geometry_oracle')
    data = json.loads(args.report.read_text())
    require(data['passed'], 'experiment did not finish')
    require(data['audit_variants'] == {
        'baseline': {'giant_batch': 16384, 'target_batch': 64},
        'candidate': {'giant_batch': 524288, 'target_batch': 2}}, 'unexpected launch geometries')
    result = dict(variants={}, ratios={}, edge_runs=[], passed=False)
    require(len(data['runs']) == 12, 'missing warm-up/measured runs')
    for repetition in range(6):
        order = ['baseline', 'candidate'] if repetition % 2 == 0 else ['candidate', 'baseline']
        pair = data['runs'][2*repetition:2*repetition+2]
        require([run['variant'] for run in pair] == order and
                all(run['repetition'] == repetition for run in pair), 'pair ordering changed')
    samples = {}
    for variant in ('baseline', 'candidate'):
        selected = copy.deepcopy(data)
        selected['runs'] = [r for r in data['runs'] if r['variant'] == variant]
        result['variants'][variant] = fleet(selected, oracle)
        samples[variant] = {}
        for run in selected['runs']:
            if not run['measured']:
                continue
            warm = [g for g in run['grants'] if not g['cold']]
            require(len(warm) == 1 and run['device_count'] == 1 and run['mode'] == 'bsgs', 'unexpected experiment workload')
            samples[variant][run['repetition']] = dict(
                process_ms=run['wall_ns']/1e6,
                grant_bodies_ms=sum(g['wall_ns'] for g in run['grants'])/1e6,
                warm_grant_ms=warm[0]['wall_ns']/1e6,
                kernel_ms=sum(g['kernel_ms'] for g in run['grants']))
    require(set(samples['baseline']) == set(samples['candidate']) == set(range(1,6)), 'missing measured pair')
    for key in next(iter(samples['baseline'].values())):
        result['ratios'][key] = dict(
            baseline=distribution([samples['baseline'][i][key] for i in range(1,6)]),
            candidate=distribution([samples['candidate'][i][key] for i in range(1,6)]),
            paired_speedup=distribution([samples['baseline'][i][key]/samples['candidate'][i][key] for i in range(1,6)]))
    edges = json.loads(args.edges.read_text())
    require(edges['passed'] and len(edges['runs']) == 6 and len(edges['commands']) == 7, 'missing edge checks')
    for record, command in zip(edges['runs'], edges['commands'][1:]):
        require(command['exit_code'] == 0, 'failed edge command')
        rows = [json.loads(line) for line in command['stdout'].splitlines()]
        require(rows[-1] == record['summary'], 'edge output mismatch')
        begin, end = int(record['begin'],16), int(record['end_exclusive'],16)
        expected = {(int(k,16),v) for k,v in record['expected_matches']}
        require(all(oracle.encode(oracle.multiply(k)) == v for k,v in expected), 'CPU-oracle target mismatch')
        cursor, matches, groups, steps = begin, [], set(), 0
        for row in rows:
            if row['type'] == 'tile':
                require(int(row['begin'],16) == cursor, 'tile gap/overlap')
                cursor = int(row['end_exclusive'],16)
            elif row['type'] == 'batch':
                require(not row['overflow'], 'unexpected overflow')
                groups.add(row['group_size'])
                steps += row['device_steps']
                matches.extend((int(m['scalar'],16),m['public_key']) for m in row['matches'])
        require(cursor == end and len(matches) == len(expected) and set(matches) == expected, 'edge result/coverage mismatch')
        require(steps == ((end-begin+256)//257)*2, 'edge target-giant work mismatch')
        require(sorted(groups) == record['actual_groups'], 'wrong group inventory')
        result['edge_runs'].append(dict(case=record['case'], variant=record['variant'], groups=sorted(groups)))
    result.update(passed=True, input_sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (args.report,args.edges,Path(__file__),Path(__file__).with_name('audit_c21.py'))})
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print('Verified five measured pairs and six oracle edge runs:', args.output)


if __name__ == '__main__':
    main()
