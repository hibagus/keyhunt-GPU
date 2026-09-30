#!/usr/bin/env python3
"""Complete HIP BSGS mapping and all-target receipts against pinned libsecp256k1."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N, P
from oracle_selftest import check_source, run as oracle_run


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--oracle',type=Path,required=True)
    p.add_argument('--report',type=Path,required=True);p.add_argument('--hip',action='store_true')
    p.add_argument('--group',choices=['auto','1','8'],default='auto');args=p.parse_args()
    binary=str(args.binary.resolve())
    report={'oracle_commit':check_source(),'seed':0xC11,'group':args.group,'cases':[],'rejections':0}
    def run(words,env=None):
        return subprocess.run([binary,'bsgs',*words],text=True,capture_output=True,timeout=90,env=env)
    def reject(words,env=None):
        r=run(words,env);assert r.returncode==2 and not r.stdout,(words,r)
        report['rejections']+=1;return r
    with tempfile.TemporaryDirectory(prefix='keyhunt-c11-') as directory:
        folder=Path(directory);target_file=folder/'targets';table_file=folder/'table'
        target_file.write_text('00'*33)
        base=['--backend','hip','--range','1:2','--targets',str(target_file),'--table',str(table_file)]
        for words in [[],base+['--device','-1'],base+['--device','2147483648'],base+['--device','0','--device','1'],
                      base+['--unknown','1'],base+['--giant-batch','0'],base+['--giant-batch','16385'],
                      base+['--target-batch','65'],base+['--target-batch','0'],base+['--group-size','2'],
                      base+['--candidate-capacity','0'],base+['--candidate-capacity','65537'],
                      base+['--reserve-bytes','1junk'],base+['--host-memory','0'],base+['--device']]:reject(words)
        for r in ['0:2','2:2','3:2','1','1:2:3',f'1:{N+1:x}',f'{N:x}:{N+1:x}']:
            reject(['--backend','hip','--range',r,'--targets',str(target_file),'--table',str(table_file)])
        if not args.hip:
            assert 'not built' in reject(base).stderr
        else:
            inventory=json.loads(subprocess.check_output([binary,'devices','--backend','hip'],text=True,timeout=30))
            assert inventory['devices'];report['inventory']=inventory
            cache={}
            for m in [1,2,3,7,17,257]:
                path=folder/f'{m}.khb'
                subprocess.run([binary,'bsgs-table','build','--m',str(m),'--output',str(path)],check=True,capture_output=True,timeout=30)
                cache[m]=path
            base[-1]=str(cache[7])
            g=oracle_run(args.oracle,['pub '+f'{1:064x}'])[0]
            for data in ['', '0', 'zz'*33, '00'*33,'04'+'00'*64,'02'+f'{P:064x}',
                         '02'+'00'*32,'06'+g[2:],g+'\x00\n','0'*100000]:
                target_file.write_text(data);reject(base)
            target_file.write_text(g)
            for words,env in [(base+['--device','2147483647'],None),(base+['--host-memory','1'],None),
                              (base+['--reserve-bytes',str(2**64-1)],None),
                              (base,dict(os.environ,HIP_VISIBLE_DEVICES='-1',ROCR_VISIBLE_DEVICES='-1'))]:reject(words,env)
            cases=[]
            def case(name,a,width,m,keys,giants=17,targets=8,capacity=64,device=0):
                cases.append(dict(name=name,a=a,width=width,m=m,keys=sorted(set(k for k in keys if 0<k<N)),
                                  giants=giants,targets=targets,capacity=capacity,device=device))
            # Every nonempty subinterval of [1,6), with every scalar and both
            # point signs as targets, exhausts the tiny finite search universe.
            for m in [1,2,3,7]:
                for a in range(1,6):
                    for end in range(a+1,7):
                        case(f'tiny_{m}_{a}_{end}',a,end-a,m,[*range(1,8),*[N-k for k in range(1,8)]],giants=2)
            for giants in [1,2,7,8,9,127,128,129,257]:
                a=(1<<128)-3;width=max(1,7*giants-2)
                case(f'giant_tail_{giants}',a,width,7,[a-1,a,a+width//2,a+width-1,a+width,N-a],giants=giants)
            for bit in [32,64,128,192,255]:
                a=(1<<bit)-5
                case(f'carry_{bit}',a,33,7,[a-1,a,a+6,a+7,a+31,a+32,a+33,N-a],giants=2,capacity=1)
            rng=random.Random(report['seed'])
            for i in range(8):
                a=rng.randrange(2,N-2048);width=rng.randrange(1,300);m=rng.choice([3,7,17,257])
                case(f'random_{i}',a,width,m,[a-1,a,a+width//2,a+width-1,a+width,N-a],capacity=2)
            case('order',N-33,33,7,[*range(N-34,N),1,2,3],giants=3,capacity=1)
            case('single_order',N-1,1,257,[N-1,1,2])
            case('no_match',1<<200,129,17,[1,2,N-1])
            case('all_targets_replay',1<<80,129,7,[*range(1<<80,(1<<80)+130)],targets=64,capacity=1)
            for device in inventory['devices']:
                a=(1<<160)-7
                case(f"device_{device['ordinal']}",a,129,17,[a,a+64,a+128,a+129,N-a],device=device['ordinal'])
            # All 20 local offset bits, plus a final partial giant. Known point
            # uniqueness proves expected results without enumerating 7M scalars.
            a=1<<200;width=7*1048576-3
            offsets=sorted({0,width-1,*[7*((1<<b)+delta)+j for b in range(20) for delta in [-1,0,1]
                                      for j in [0,6] if 0<=7*((1<<b)+delta)+j<width]})
            case('maximum_giant_bits',a,width,7,[a+d for d in offsets]+[a+width],giants=1048576,targets=1)
            # Exhaustively derive every searched scalar for ordinary cases, plus
            # every target. Neither CPU implementation under test defines expected hits.
            scalars=set(k for c in cases for k in c['keys'])
            scalars.update(k for c in cases if c['name']!='maximum_giant_bits' for k in range(c['a'],c['a']+c['width']))
            ordered=sorted(scalars);pub=dict(zip(ordered,oracle_run(args.oracle,[f'pub {k:064x}' for k in ordered])))
            report['oracle_public_keys']=len(pub)
            for c in cases:
                unique=sorted(pub[k] for k in c['keys'])
                lines=[]
                for i,point in enumerate(reversed(unique)):
                    lines.append((('03' if int(point[-2:],16)&1 else '02')+point[2:66]) if i%2 else point)
                target_file.write_text('\r\n'.join(lines+[unique[0]])+'\r\n\r\n')
                words=['--backend','hip','--range',f"{c['a']:x}:{c['a']+c['width']:x}",'--targets',str(target_file),
                       '--table',str(cache[c['m']]),'--giant-batch',str(c['giants']),'--target-batch',str(c['targets']),
                       '--candidate-capacity',str(c['capacity']),'--device',str(c['device']),'--group-size',str(args.group)]
                r=run(words);assert r.returncode==0 and not r.stderr,(c,r)
                records=[json.loads(line) for line in r.stdout.splitlines()]
                start=records[0];assert start['type']=='start' and start['target_count']==len(unique)
                assert start['target_digest']==hashlib.sha256(b'bsgs-targets-v1\0'+b''.join(bytes.fromhex(q) for q in unique)).hexdigest()
                cursor=c['a'];first=0;actual=[];steps=0;verified=0;overflows=0;launches=0;tiles=0;tail=0
                for record in records[1:-1]:
                    assert int(record['begin'],16)==cursor,(c,record,cursor)
                    end=int(record['end_exclusive'],16)
                    if record['type']=='tile':
                        assert first==len(unique) and record['targets_completed']==first
                        assert not record['durable_coverage'];cursor=end;first=0;tiles+=1;continue
                    assert record['type']=='batch' and record['first_target']==first
                    assert record['giants_per_target']==(end-cursor+c['m']-1)//c['m']
                    assert record['device_steps']==record['giants_per_target']*record['target_count']
                    assert record['group_size'] in (1,8)
                    if args.group!='auto':assert record['group_size']==int(args.group)
                    steps+=record['device_steps'];tail+=record['tail_rejections'];launches+=1
                    if record['overflow']:
                        assert record['verified_steps']==0 and not record['matches'] and record['candidate_count']>c['capacity']
                        overflows+=1
                    else:
                        assert record['verified_steps']==record['device_steps'];verified+=record['verified_steps']
                        for match in record['matches']:
                            assert first<=match['target']<first+record['target_count']
                            actual.append((int(match['scalar'],16),match['public_key'],match['target']))
                        first+=record['target_count']
                expected=sorted((k,pub[k],unique.index(pub[k])) for k in c['keys'] if c['a']<=k<c['a']+c['width'])
                assert sorted(actual)==expected,(c,actual,expected)
                summary=records[-1]
                assert summary['type']=='summary' and summary['complete'] and not summary['durable_coverage']
                assert cursor==c['a']+c['width'] and int(summary['verified_scalars'],16)==c['width']
                assert int(summary['device_steps'],16)==steps and int(summary['verified_target_steps'],16)==verified
                assert int(summary['matches'],16)==len(expected) and summary['launch_count']==launches
                assert summary['overflow_replays']==overflows and summary['tiles']==tiles
                if c['name']=='all_targets_replay':assert overflows>0
                if c['name'].startswith('giant_tail_'):assert tail>0
                report['cases'].append({**{k:v for k,v in c.items() if k!='keys'},'expected_matches':len(expected),'summary':summary})
            with open('/dev/full','w') as sink:
                r=subprocess.run([binary,'bsgs',*words],text=True,stdout=sink,stderr=subprocess.PIPE,timeout=30)
                assert r.returncode==2 and 'failed to write BSGS output' in r.stderr
                report['rejections']+=1
    report['binary_sha256']=hashlib.sha256(args.binary.read_bytes()).hexdigest()
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"BSGS CLI group {args.group}: {len(report['cases'])} independent cases, {report['rejections']} rejections")

if __name__=='__main__':main()
