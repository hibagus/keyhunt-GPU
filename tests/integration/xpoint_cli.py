#!/usr/bin/env python3
"""Exact HIP search coverage and replay, checked against pinned libsecp256k1."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N, P
from oracle_selftest import check_source, run as oracle_run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--hip', action='store_true')
    parser.add_argument('--kernel', choices=['direct','stepped'], default='stepped')
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    report = {'oracle_commit': check_source(), 'seed': 0xC09, 'kernel': args.kernel, 'cases': [], 'rejections': 0}

    def run(words, env=None):
        return subprocess.run([binary, 'xpoint', *words], text=True, capture_output=True, timeout=90, env=env)

    with tempfile.TemporaryDirectory(prefix='keyhunt-c09-') as directory:
        target_file = Path(directory)/'targets.txt'
        target_file.write_text('00'*32+'\n')
        base = ['--backend', 'hip', '--range', '1:2', '--targets', str(target_file)]
        # Malformed/ambiguous options and exact scalar domains fail before launch.
        invalid = [[], ['--backend', 'cuda'], base+['--unknown', '1'], base+['--device', '-1'],
                   base+['--device', '2147483648'], base+['--device', '0', '--device', '0'],
                   base+['--batch-size', '0'], base+['--batch-size', '1048577'],
                   base+['--batch-size', '12junk'], base+['--candidate-capacity', '0'],
                   base+['--candidate-capacity', '1048577'], base+['--kernel','unknown'], base+['--device']]
        invalid += [['--backend','hip','--range',r,'--targets',str(target_file)]
                    for r in ['0:2','2:2','3:2','1','1:2:3',f'1:{N+1:x}',f'{N:x}:{N+1:x}']]
        for words in invalid:
            result = run(words)
            assert result.returncode == 2 and not result.stdout, result
            report['rejections'] += 1
        if not args.hip:
            result = run(base)
            assert result.returncode == 2 and 'not built' in result.stderr and not result.stdout, result
            report['rejections'] += 1
        else:
            for data in ['', '0\n', 'zz'*32+'\n', f'{P:064x}\n', '0'*67, '0'*64+'\x00\n', '0'*100000]:
                target_file.write_text(data)
                result = run(base)
                assert result.returncode == 2 and not result.stdout, result
                report['rejections'] += 1
            target_file.write_text('00'*32+'\n')
            for words, env in [(base+['--device','2147483647'],None),
                               (base,dict(os.environ,HIP_VISIBLE_DEVICES='-1',ROCR_VISIBLE_DEVICES='-1'))]:
                result = run(words,env)
                assert result.returncode == 2 and 'not visible' in result.stderr and not result.stdout, result
                report['rejections'] += 1
            inventory = json.loads(subprocess.check_output([binary,'devices','--backend','hip'],text=True,timeout=30))
            assert inventory['devices'], 'real HIP hardware required'
            report['inventory'] = inventory
            cases = []
            for count in [1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025]:
                cases.append((f'dense_{count}',(1<<80)-3,count,'dense',2049,2049,0))
            for bit in [32,64,128,192,255]:
                cases.append((f'carry_{bit}',(1<<bit)-5,19,'boundary',7,2,0))
            cases += [('low',1,19,'boundary',8,2,0), ('order',N-33,33,'dense',33,4,0),
                      ('single_order',N-1,1,'boundary',1,1,0),
                      ('no_match',1<<200,129,'none',128,1,0),
                      ('dense_replay',1<<128,129,'dense',129,1,0),
                      ('full_x',1<<192,9,'near',9,9,0)]
            rng=random.Random(report['seed'])
            for i in range(8):
                cases.append((f'random_{i}',rng.randrange(2,N-257),rng.randrange(1,65),'boundary',17,2,0))
            for device in inventory['devices']:
                cases.append((f"device_{device['ordinal']}",(1<<160)-7,129,'small',129,3,device['ordinal']))
            # Every searched scalar, plus external boundary targets, comes from the
            # independent pinned native oracle. Python only compares exact strings.
            scalars = sorted({k for _,begin,count,_,_,_,_ in cases for k in range(max(1,begin-1),min(N,begin+count+1))})
            public = dict(zip(scalars,oracle_run(args.oracle,[f'pub {k:064x}' for k in scalars])))
            xs = {k:pub[2:66] for k,pub in public.items()}
            for name,begin,count,kind,batch,capacity,device in cases:
                if kind == 'small': targets=[xs[k] for k in [begin,begin+count//2,begin+count-1]]
                elif kind == 'dense': targets=[xs[k] for k in range(begin,begin+count)]
                elif kind == 'none': targets=['00'*32]
                elif kind == 'near': targets=[f'{int(xs[begin],16)^1:064x}']
                else: targets=[xs[k] for k in [begin,begin+count//2,begin+count-1,max(1,begin-1),min(N-1,begin+count)]]
                # Duplicate and unordered input lines cannot multiply candidate count.
                target_file.write_text('\r\n'.join(list(reversed(targets))+[targets[0]])+'\r\n\r\n')
                unique=sorted(set(targets))
                expected=[(k,xs[k],unique.index(xs[k])) for k in range(begin,begin+count) if xs[k] in unique]
                result=run(['--backend','hip','--range',f'{begin:x}:{begin+count:x}','--targets',str(target_file),
                            '--batch-size',str(batch),'--candidate-capacity',str(capacity),'--device',str(device),'--kernel',args.kernel])
                assert result.returncode == 0 and not result.stderr, (name,result)
                records=[json.loads(line) for line in result.stdout.splitlines()]
                assert records[0]['type']=='start' and records[-1]['type']=='summary'
                assert records[0]['target_count']==len(unique) and not records[0]['durable_coverage']
                cursor=begin; actual=[]; executed=0; overflows=0
                for record in records[1:-1]:
                    assert record['type']=='batch' and int(record['begin'],16)==cursor, (name,record,cursor)
                    executed+=record['device_steps']
                    assert record['device_steps']==int(record['end_exclusive'],16)-cursor
                    if record['overflow']:
                        assert record['verified_steps']==0 and not record['matches']
                        assert record['candidate_count']>capacity
                        overflows+=1
                    else:
                        assert record['verified_steps']==record['device_steps']
                        actual += [(int(m['scalar'],16),m['x'],m['target']) for m in record['matches']]
                        cursor=int(record['end_exclusive'],16)
                summary=records[-1]
                assert cursor==begin+count and actual==expected, (name,actual,expected)
                assert summary['complete'] and not summary['durable_coverage']
                assert int(summary['verified_steps'],16)==count and int(summary['device_steps'],16)==executed
                assert int(summary['matches'],16)==len(expected) and summary['launch_count']==len(records)-2
                assert summary['overflow_replays']==overflows
                if name in ('dense_replay','order'): assert overflows>0
                report['cases'].append({'name':name,'device':device,'begin':hex(begin),'count':count,
                    'targets':len(unique),'expected_matches':len(expected),'summary':summary})
            # Exercise every local-offset table bit at the maximum batch size.
            # For these known Xs, the only other scalar is n-k, outside this job;
            # this proves the expected hit set without materializing a million keys.
            begin=1<<200; count=1048576
            offsets=sorted({0,count-1,*[d for bit in range(20) for d in [(1<<bit)-1,1<<bit,(1<<bit)+1] if d<count]})
            pubs=oracle_run(args.oracle,[f'pub {begin+d:064x}' for d in offsets])
            targets=[pub[2:66] for pub in pubs]
            target_file.write_text('\n'.join(targets)) # also check no final newline
            result=run(['--backend','hip','--range',f'{begin:x}:{begin+count:x}','--targets',str(target_file),
                        '--batch-size',str(count),'--candidate-capacity',str(len(targets)),'--kernel',args.kernel])
            assert result.returncode==0 and not result.stderr, result
            records=[json.loads(line) for line in result.stdout.splitlines()]
            assert len(records)==3 and records[1]['verified_steps']==count and not records[1]['overflow']
            assert [(int(m['scalar'],16),m['x']) for m in records[1]['matches']]==list(zip([begin+d for d in offsets],targets))
            assert int(records[-1]['verified_steps'],16)==count and records[-1]['complete']
            report['cases'].append({'name':'maximum_batch_offset_bits','device':0,'begin':hex(begin),
                'count':count,'targets':len(targets),'expected_matches':len(targets),'summary':records[-1]})
    report['binary_sha256']=hashlib.sha256(args.binary.read_bytes()).hexdigest()
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"Xpoint CLI: {len(report['cases'])} independent search cases, {report['rejections']} rejections")

if __name__=='__main__': main()
