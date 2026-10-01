#!/usr/bin/env python3
"""Independent exact P2PKH text-prefix oracle, including overlapping relations."""
import argparse,hashlib,json,random,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N
from oracle_selftest import check_source,run as oracle_run
from hash160 import hash160,address
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--oracle',type=Path,required=True)
p.add_argument('--report',type=Path,required=True);p.add_argument('--hardware',action='store_true')
p.add_argument('--backend',choices=['hip','cuda'],default='hip');p.add_argument('--kernel',choices=['direct','stepped'],default='stepped')
a=p.parse_args();binary=str(a.binary.resolve())
report={'oracle_commit':check_source(),'kernel':a.kernel,'cases':[],'rejections':0}
def target(tag,prefix):return bytes([tag,len(prefix)])+prefix.encode()+bytes(34-len(prefix))
def run(words):return subprocess.run([binary,'vanity',*map(str,words)],text=True,capture_output=True,timeout=180)
with tempfile.TemporaryDirectory(prefix='kh-vanity-') as directory:
    file=Path(directory)/'prefixes';file.write_text('1\n')
    base=['--backend',a.backend,'--range','1:2','--targets',file]
    invalid=[[],base+['--encoding','invalid'],base+['--device','-1'],base+['--kernel','invalid'],
        base+['--batch-size','0'],base+['--batch-size','1048577'],base+['--candidate-capacity','0'],
        base+['--candidate-capacity','1'],base+['--candidate-capacity','1048577'],
        base+['--encoding','both','--encoding','compressed'],base+['--unknown','1']]
    invalid += [['--backend',a.backend,'--range',r,'--targets',file] for r in ('0:2','2:2','3:2','1','1:2:3',f'1:{N+1:x}')]
    for words in invalid:
        result=run(words);assert result.returncode==2 and not result.stdout,result;report['rejections']+=1
    if not a.hardware:
        result=run(base);assert result.returncode==2 and 'not built' in result.stderr and not result.stdout,result
    else:
        for data in ('','0','1O','1I','1l','1 B',' 1','1\0','1'*35,'3ABC','bc1q','0x123','a'*100000,'1\n'*2049):
            file.write_text(data);result=run(base);assert result.returncode==2 and not result.stdout,result;report['rejections']+=1
        inventory=json.loads(subprocess.check_output([binary,'devices','--backend',a.backend],text=True));report['inventory']=inventory
        cases=[(f'dense_{n}',(1<<80)-3,n,'broad' if n>512 else 'dense','both',2049,4096,0)
               for n in (1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025)]
        cases += [(f'carry_{bit}',(1<<bit)-5,19,'boundary','both',7,0,0) for bit in (32,64,128,192,255)]
        cases += [('low',1,19,'boundary','compressed',8,0,0),('order',N-33,33,'dense','both',33,0,0),
                  ('last',N-1,1,'dense','uncompressed',1,0,0),('none',1<<200,129,'none','both',128,0,0),
                  ('dense_replay',1<<128,129,'broad','both',129,0,0),('dense_prefix',1<<128,2049,'prefix','both',256,0,0),
                  ('overlap',1,65,'overlap','both',65,0,0),('leading_zeroes',1,257,'zeroes','both',128,0,0)]
        rng=random.Random(0xC23B)
        cases += [(f'random_{i}',rng.randrange(2,N-257),rng.randrange(1,65),'boundary',
                   'compressed' if i%2 else 'uncompressed',17,0,0) for i in range(8)]
        cases += [(f"device_{d['ordinal']}",(1<<160)-7,129,'boundary','both',129,16,d['ordinal']) for d in inventory['devices']]
        scalars=sorted({k for _,begin,count,*_ in cases for k in range(max(1,begin-1),min(N,begin+count+1))})
        public=dict(zip(scalars,oracle_run(a.oracle,[f'pub {k:064x}' for k in scalars])))
        addresses={(k,t):address(hash160(pub,t)) for k,pub in public.items() for t in (1,2)}
        for name,begin,count,kind,encoding,batch,capacity,device in cases:
            tags=(1,2) if encoding=='both' else (1,) if encoding=='compressed' else (2,)
            selected=range(begin,begin+count) if kind=='dense' else range(begin,begin+4) if kind=='prefix' else (
                begin,begin+count//2,begin+count-1,max(1,begin-1),min(N-1,begin+count))
            raw=(['1'*34] if kind=='none' else ['1'] if kind=='broad' else ['11','111','1111'] if kind=='zeroes'
                 else ['1','1B','1b','1Bg','1EH',addresses[1,1],addresses[1,2]] if kind=='overlap'
                 else [addresses[k,t] for k in selected for t in tags])
            targets=sorted({target(t,v) for v in raw for t in tags});index={v:i for i,v in enumerate(targets)}
            factor=sum(len({v[1] for v in targets if v[0]==t}) for t in tags)
            capacity=capacity or factor
            expected=[]
            for k in range(begin,begin+count):
                for t,v in enumerate(targets):
                    prefix=v[2:2+v[1]].decode();actual=addresses[k,v[0]]
                    if actual.startswith(prefix):expected.append((k,v[0],actual,prefix,t))
            file.write_text('\r\n'.join(list(reversed(raw))+[raw[0]])+'\r\n\r\n')
            result=run(['--backend',a.backend,'--range',f'{begin:x}:{begin+count:x}','--targets',file,
                        '--encoding',encoding,'--batch-size',batch,'--candidate-capacity',capacity,'--device',device,'--kernel',a.kernel])
            assert result.returncode==0 and not result.stderr,(name,result)
            records=[json.loads(line) for line in result.stdout.splitlines()]
            assert records[0]['mode']=='vanity' and records[0]['target_count']==len(targets)
            assert records[0]['target_digest']==hashlib.sha256(b'vanity-p2pkh-v1\0'+b''.join(targets)).hexdigest()
            cursor=begin;actual=[];executed=overflows=0
            for row in records[1:-1]:
                assert int(row['begin'],16)==cursor and row['device_steps']==int(row['end_exclusive'],16)-cursor
                executed+=row['device_steps']
                if row['overflow']:
                    assert not row['verified_steps'] and not row['matches'] and row['candidate_count']>capacity;overflows+=1
                else:
                    assert row['verified_steps']==row['device_steps']
                    actual += [(int(m['scalar'],16),1 if m['encoding']=='compressed' else 2,m['address'],m['prefix'],m['target']) for m in row['matches']]
                    cursor=int(row['end_exclusive'],16)
            summary=records[-1]
            assert cursor==begin+count and actual==expected,(name,actual,expected)
            assert summary['complete'] and not summary['durable_coverage']
            assert int(summary['verified_steps'],16)==count and int(summary['device_steps'],16)==executed
            assert int(summary['matches'],16)==len(expected) and summary['overflow_replays']==overflows
            if name in ('dense_replay','dense_prefix','order','overlap'):assert overflows>0
            if name=='dense_prefix':assert summary['launch_count']<40
            report['cases'].append({'name':name,'device':device,'count':count,'bound':factor,'summary':summary})
        begin=1<<200;count=1048576
        offsets=sorted({0,count-1,*[o for bit in range(20) for o in ((1<<bit)-1,1<<bit,(1<<bit)+1) if o<count]})
        prefixes=[address(hash160(pub,1)) for pub in oracle_run(a.oracle,[f'pub {begin+o:064x}' for o in offsets])]
        file.write_text('\n'.join(prefixes))
        result=run(['--backend',a.backend,'--range',f'{begin:x}:{begin+count:x}','--targets',file,'--encoding','compressed',
                    '--batch-size',count,'--candidate-capacity',128,'--kernel',a.kernel])
        assert result.returncode==0 and not result.stderr,result
        records=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(records)==3 and records[1]['verified_steps']==count and not records[1]['overflow']
        assert [(int(m['scalar'],16),m['address']) for m in records[1]['matches']]==list(zip([begin+o for o in offsets],prefixes))
        report['cases'].append({'name':'maximum_batch_offset_bits','count':count,'summary':records[-1]})
report['passed']=True;report['binary_sha256']=hashlib.sha256(a.binary.read_bytes()).hexdigest()
a.report.write_text(json.dumps(report,indent=2)+'\n');print(f"Vanity: {len(report['cases'])} independent cases, {report['rejections']} rejections")
