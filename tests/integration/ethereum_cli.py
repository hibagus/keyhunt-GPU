#!/usr/bin/env python3
"""Native Ethereum search against pinned libsecp256k1 and PyCryptodome."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N
from oracle_selftest import check_source, run as oracle_run
from ethereum import address, checksum
parser=argparse.ArgumentParser()
parser.add_argument('--binary',type=Path,required=True)
parser.add_argument('--oracle',type=Path,required=True)
parser.add_argument('--report',type=Path,required=True)
parser.add_argument('--hardware',action='store_true')
parser.add_argument('--backend',choices=['hip','cuda'],default='hip')
parser.add_argument('--kernel',choices=['direct','stepped'],default='stepped')
args=parser.parse_args()
binary=str(args.binary.resolve())
report={'oracle_commit':check_source(),'hash_oracle':'PyCryptodome 3.23.0','kernel':args.kernel,'cases':[],'rejections':0}
def run(words):
    return subprocess.run([binary,'ethereum',*words],text=True,capture_output=True,timeout=180)
with tempfile.TemporaryDirectory(prefix='keyhunt-ethereum-') as directory:
    target_file=Path(directory)/'targets.txt';target_file.write_text('00'*20+'\n')
    base=['--backend',args.backend,'--range','1:2','--targets',str(target_file)]
    invalid=[[],base+['--encoding','compressed'],base+['--device','-1'],base+['--kernel','invalid'],
        base+['--batch-size','0'],base+['--batch-size','1048577'],base+['--candidate-capacity','0'],
        base+['--candidate-capacity','1048577'],base+['--kernel','direct','--kernel','stepped'],base+['--unknown','1']]
    invalid += [['--backend',args.backend,'--range',r,'--targets',str(target_file)]
                for r in ('0:2','2:2','3:2','1','1:2:3',f'1:{N+1:x}')]
    for words in invalid:
        result=run(words);assert result.returncode==2 and not result.stdout,result
        report['rejections']+=1
    if not args.hardware:
        result=run(base);assert result.returncode==2 and 'not built' in result.stderr and not result.stdout,result
    else:
        for data in ('','0','zz'*20,'00'*21,'0'*40+'\0','a'*100000,'0X'+'0'*40,
                     '0x7e5F4552091A69125d5DfCb7b8C2659029395Bdf','1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH'):
            target_file.write_text(data);result=run(base)
            assert result.returncode==2 and not result.stdout,result
            report['rejections']+=1
        inventory=json.loads(subprocess.check_output([binary,'devices','--backend',args.backend],text=True))
        report['inventory']=inventory
        # Dense cases prove every scalar through group/workgroup tails, not just
        # sparse target hits. Carry and curve-order cases forbid modular wrapping.
        cases=[(f'dense_{n}',(1<<80)-3,n,'dense',2049,2049,0)
               for n in (1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025)]
        cases += [(f'carry_{bit}',(1<<bit)-5,19,'boundary',7,1,0) for bit in (32,64,128,192,255)]
        cases += [('low',1,19,'boundary',8,1,0),('order',N-33,33,'dense',33,1,0),
                  ('last',N-1,1,'dense',1,1,0),('no_match',1<<200,129,'none',128,1,0),
                  ('dense_replay',1<<128,129,'dense',129,1,0),('dense_prefix',1<<128,2049,'prefix',256,1,0)]
        rng=random.Random(0xC23E)
        cases += [(f'random_{i}',rng.randrange(2,N-257),rng.randrange(1,65),'boundary',17,2,0) for i in range(8)]
        cases += [(f"device_{d['ordinal']}",(1<<160)-7,129,'boundary',129,6,d['ordinal']) for d in inventory['devices']]
        scalars=sorted({k for _,begin,count,*_ in cases for k in range(max(1,begin-1),min(N,begin+count+1))})
        public=oracle_run(args.oracle,[f'pub {k:064x}' for k in scalars])
        hashes=dict(zip(scalars,map(address,public)))
        for case_no,(name,begin,count,kind,batch,capacity,device) in enumerate(cases):
            selected=range(begin,begin+count) if kind=='dense' else range(begin,begin+4) if kind=='prefix' else (
                begin,begin+count//2,begin+count-1,max(1,begin-1),min(N-1,begin+count))
            raw=['00'*20] if kind=='none' else [hashes[k] for k in selected]
            targets=sorted(set(raw));index={h:i for i,h in enumerate(targets)}
            expected=[(k,'0x'+hashes[k],index[hashes[k]]) for k in range(begin,begin+count) if hashes[k] in index]
            forms=(lambda h:h,lambda h:'0x'+h,lambda h:'0x'+h.upper(),checksum)
            lines=[forms[case_no%4](h) for h in raw]
            target_file.write_text('\r\n'.join(list(reversed(lines))+[lines[0]])+'\r\n\r\n')
            result=run(['--backend',args.backend,'--range',f'{begin:x}:{begin+count:x}','--targets',str(target_file),
                        '--batch-size',str(batch),'--candidate-capacity',str(capacity),'--device',str(device),'--kernel',args.kernel])
            assert result.returncode==0 and not result.stderr,(name,result)
            records=[json.loads(line) for line in result.stdout.splitlines()]
            assert records[0]['mode']=='ethereum' and records[0]['target_count']==len(targets)
            canonical=b'ethereum-v1\0'+b''.join(bytes.fromhex(h) for h in targets)
            assert records[0]['target_digest']==hashlib.sha256(canonical).hexdigest()
            cursor=begin;actual=[];executed=overflows=0
            for row in records[1:-1]:
                assert row['type']=='batch' and int(row['begin'],16)==cursor
                assert row['device_steps']==int(row['end_exclusive'],16)-cursor
                executed+=row['device_steps']
                if row['overflow']:
                    assert row['verified_steps']==0 and not row['matches'] and row['candidate_count']>capacity
                    overflows+=1
                else:
                    assert row['verified_steps']==row['device_steps']
                    actual += [(int(m['scalar'],16),m['address'],m['target']) for m in row['matches']]
                    cursor=int(row['end_exclusive'],16)
            summary=records[-1]
            assert cursor==begin+count and actual==expected,(name,actual,expected)
            assert summary['complete'] and not summary['durable_coverage']
            assert int(summary['verified_steps'],16)==count and int(summary['device_steps'],16)==executed
            assert int(summary['matches'],16)==len(expected) and summary['launch_count']==len(records)-2
            assert summary['overflow_replays']==overflows
            if name in ('dense_replay','order','dense_prefix'):assert overflows>0
            if name=='dense_prefix':assert summary['launch_count']<40
            report['cases'].append({'name':name,'device':device,'begin':hex(begin),'count':count,'targets':len(targets),'summary':summary})
        begin=1<<200;count=1048576
        offsets=sorted({0,count-1,*[o for bit in range(20) for o in ((1<<bit)-1,1<<bit,(1<<bit)+1) if o<count]})
        hashes=list(map(address,oracle_run(args.oracle,[f'pub {begin+o:064x}' for o in offsets])))
        target_file.write_text('\n'.join(map(checksum,hashes)))
        result=run(['--backend',args.backend,'--range',f'{begin:x}:{begin+count:x}','--targets',str(target_file),
                    '--batch-size',str(count),'--candidate-capacity',str(len(hashes)),'--kernel',args.kernel])
        assert result.returncode==0 and not result.stderr,result
        records=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(records)==3 and records[1]['verified_steps']==count and not records[1]['overflow']
        assert [(int(m['scalar'],16),m['address']) for m in records[1]['matches']]==list(zip([begin+o for o in offsets],['0x'+h for h in hashes]))
        report['cases'].append({'name':'maximum_batch_offset_bits','count':count,'summary':records[-1]})
report['binary_sha256']=hashlib.sha256(args.binary.read_bytes()).hexdigest();report['passed']=True
args.report.write_text(json.dumps(report,indent=2)+'\n')
print(f"Ethereum CLI: {len(report['cases'])} independent searches, {report['rejections']} rejections")
