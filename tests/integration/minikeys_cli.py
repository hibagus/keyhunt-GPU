#!/usr/bin/env python3
"""Independent exhaustive candidate oracle; ordinals and private scalars never mix."""
import argparse,hashlib,json,random,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from minikey import text,ordinal,scalar,PUBLIC_KEYS
from minikey_random_window import RandomWindow
from oracle_selftest import check_source,run as oracle_run
from hash160 import hash160,address
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',choices=('hip','cuda'),default='hip')
orders=p.add_mutually_exclusive_group();orders.add_argument('--reverse',action='store_true');orders.add_argument('--both-ends',action='store_true');orders.add_argument('--dance',action='store_true');orders.add_argument('--random-window',action='store_true')
a=p.parse_args();order='random-window' if a.random_window else 'dance' if a.dance else 'both-ends' if a.both_ends else 'reverse' if a.reverse else 'forward';binary=str(a.binary.resolve());report=dict(oracle_commit=check_source(),cases=[],rejections=0,inspections=0,hardware=a.hardware,reverse=a.reverse,ordinal_order=order)
def invoke(words,seed=42,window=4):return subprocess.run([binary,'minikeys',*map(str,words),*(['--ordinal-order',order] if order!='forward' and (not words or words[0]!='inspect') else []),*(['--ordinal-seed',f'{seed:x}','--ordinal-window',str(window)] if a.random_window and seed is not None and (not words or words[0]!='inspect') else [])],capture_output=True,text=True,timeout=150)
for key in (*PUBLIC_KEYS,'S'+'1'*21,'S'+'z'*29):
    result=invoke(['inspect','--key',key]);assert result.returncode==0,result
    value=json.loads(result.stdout);private=scalar(key)
    assert int(value['ordinal'],16)==ordinal(key) and value['valid']==bool(private)
    assert value['coordinate_space']=='minikey-ordinal-v1' and value['length']==len(key)
    if private:assert int(value['scalar'],16)==private
    else:assert 'scalar' not in value
    assert int(value['space_end_exclusive'],16)==58**(len(key)-1)+1
    report['inspections']+=1
with tempfile.TemporaryDirectory(prefix='kh-minikeys-') as temporary:
    file=Path(temporary)/'targets.txt';file.write_text('1CciesT23BNionJeXrbxmjc7ywfiyM4oLW\n')
    base=['--backend',a.backend,'--length','30','--range','1:2','--targets',file]
    for invalid_order in ('random','backward',''):
        rejected=subprocess.run([binary,'minikeys',*map(str,base),'--ordinal-order',invalid_order],capture_output=True,text=True,timeout=30)
        assert rejected.returncode==2 and 'ordinal-order must be' in rejected.stderr and not rejected.stdout
        report['rejections']+=1
    for flag,value in (('--ordinal-window','0'),('--ordinal-window','257'),('--ordinal-window','x'),('--ordinal-seed','z'),('--ordinal-seed','1'+'0'*64)):
        rejected=subprocess.run([binary,'minikeys',*map(str,base),'--ordinal-order','random-window',flag,value],capture_output=True,text=True,timeout=30)
        assert rejected.returncode==2 and not rejected.stdout
        report['rejections']+=1
    for old in ('forward','reverse','both-ends','dance'):
        for flag,value in (('--ordinal-seed','0'),('--ordinal-window','64')):
            rejected=subprocess.run([binary,'minikeys',*map(str,base),'--ordinal-order',old,flag,value],capture_output=True,text=True,timeout=30)
            assert rejected.returncode==2 and 'require ordinal-order random-window' in rejected.stderr
            report['rejections']+=1
    invalid=[[],base+['--kernel','stepped'],base+['--encoding','bad'],base+['--input-format','bad'],base+['--device','-1'],
             base+['--batch-size','0'],base+['--batch-size','1048577'],base+['--candidate-capacity','0'],base+['--candidate-capacity','1'],
             base+['--candidate-capacity','1048577'],base+['--length','22'],base+['--unknown','1'],['inspect','--key','S'+'1'*25]]
    invalid += [['--backend',a.backend,'--length',length,'--range','1:2','--targets',file] for length in ('','0','21','23','26','31','18446744073709551616')]
    invalid += [['--backend',a.backend,'--length','22','--range',bounds,'--targets',file]
                for bounds in ('0:2','2:2','3:2','1','1:2:3',f'1:{58**21+2:x}')]
    for words in invalid:
        result=invoke(words);assert result.returncode==2 and not result.stdout,(words,result);report['rejections']+=1
    if not a.hardware:
        result=invoke(base);assert result.returncode==2 and 'not built' in result.stderr
    else:
        for data in ('',' 1CciesT23BNionJeXrbxmjc7ywfiyM4oLW','1O','1'*100000,'00'*20,'1CciesT23BNionJeXrbxmjc7ywfiyM4oLX'):
            file.write_text(data);result=invoke(base);assert result.returncode==2 and not result.stdout,result;report['rejections']+=1
        inventory=json.loads(subprocess.check_output([binary,'devices','--backend',a.backend],text=True));report['inventory']=inventory
        cases=[];rng=random.Random(0xC236)
        for length,key in zip((22,30),PUBLIC_KEYS):
            begin=ordinal(key);end=58**(length-1)+1
            cases += [(f'tail_{count}_{length}',length,begin,count,'both','address',2049,4096,0)
                      for count in (1,2,3,4,31,32,33,127,128,129,1023,1024,1025)]
            cases += [(f'base58_carry_{power}_{length}',length,58**power-2,1025,'both','hash160',257,2,0)
                      for power in (1,2,5,10,length-2)]
            cases += [(f'limb_{bit}_{length}',length,2**bit-3,1025,'both','address',1025,2,0)
                      for bit in (32,64,96,128,160) if 2**bit<end]
            cases += [(f'first_{length}',length,1,1025,'both','address',257,16,0),
                      (f'last_{length}',length,end-1025,1025,'both','hash160',257,16,0),
                      (f'rejected_{length}',length,1,1,'both','address',1,2,0),
                      (f'overflow_{length}',length,begin,4097,'both','address',4097,2,0)]
            cases += [(f'random_{i}_{length}',length,rng.randrange(2,end-1025),1025,
                       'compressed' if i%2 else 'uncompressed','hash160',129,1,0) for i in range(4)]
            # All admitted candidates in a full launch are targets. This detects
            # skipped valid ordinals, not only false positives at selected indices.
            cases.append((f'maximum_batch_{length}',length,1,1048576,'both','hash160',1048576,32768,0))
        if a.both_ends or a.dance or a.random_window:
            for length,key in zip((22,30),PUBLIC_KEYS):
                cases += [(f'meeting_{count}_{batch}_{length}',length,ordinal(key),count,'both','hash160',batch,4096,0) for count,batch in ((3,2),(35,17),(257,129),(33,1))]
        if a.random_window:
            for length,key in zip((22,30),PUBLIC_KEYS):
                cases += [(f'window_{window}_{length}',length,ordinal(key),window*17+3,'both','hash160',17,4096,0) for window in (1,2,3,64,256)]
                cases += [(f'defaults_{length}',length,ordinal(key),1107,'both','address',17,4096,0)]
        cases += [(f"device_{d['ordinal']}",22 if d['ordinal']%2==0 else 30,ordinal(PUBLIC_KEYS[d['ordinal']%2]),1025,
                   'both','address',1025,16,d['ordinal']) for d in inventory['devices']]
        for name,length,begin,count,encoding,form,batch,capacity,device in cases:
            tags=(1,2) if encoding=='both' else (1,) if encoding=='compressed' else (2,)
            admitted={o:scalar(text(o,length)) for o in range(begin,begin+count)}
            admitted={o:s for o,s in admitted.items() if s}
            # Keep a nonmatching canonical target when the whole range is rejected.
            seeds=sorted(set(admitted.values()) or {1})
            public=dict(zip(seeds,oracle_run(a.oracle,[f'pub {s:064x}' for s in seeds])))
            hashes={(s,t):hash160(pub,t) for s,pub in public.items() for t in tags}
            raw=list(hashes.values());file.write_text('\n'.join(address(v) if form=='address' else v for v in list(reversed(raw))+[raw[0]]))
            targets=sorted({bytes([length,t])+bytes.fromhex(v) for v in raw for t in tags});indices={v:i for i,v in enumerate(targets)}
            expected=sorted((o,s,text(o,length),t,hashes[s,t],indices[bytes([length,t])+bytes.fromhex(hashes[s,t])])
                            for o,s in admitted.items() for t in tags)
            seed,window=42,4
            if name.startswith('window_'):seed,window=2**256-1,int(name.split('_')[1])
            if name.startswith('defaults_'):seed,window=None,64
            result=invoke(['--backend',a.backend,'--length',length,'--range',f'{begin:x}:{begin+count:x}','--targets',file,
                           '--encoding',encoding,'--input-format',form,'--batch-size',batch,'--candidate-capacity',capacity,'--device',device],seed=seed,window=window)
            assert result.returncode==0 and not result.stderr,(name,result.stderr)
            records=[json.loads(line) for line in result.stdout.splitlines()];start=records[0]
            assert start['mode']=='minikeys' and start['coordinate_space']=='minikey-ordinal-v1'
            assert start['target_digest']==hashlib.sha256(b'minikeys-v1\0'+b''.join(targets)).hexdigest() and start['target_count']==len(targets)
            pivot=begin+count//2
            remaining=[(begin,pivot),(pivot,begin+count)] if a.dance and pivot>begin else [(begin,begin+count)]
            actual=[];attempts=overflows=phase=0;sequence=[]
            model=RandomWindow([(begin,begin+count)],seed or 0,window);limit=batch
            for row in records[1:-1]:
                reverse=order=='reverse' or (a.both_ends and phase%2==1) or (a.dance and phase%3==1)
                chosen=len(remaining)-1 if reverse else 0
                if a.dance and phase%3==2:chosen=next((i for i,v in enumerate(remaining) if v[0]>=pivot),0)
                low,high=remaining[chosen]
                if a.random_window:
                    planned=model.plan(batch,limit);assert planned is not None
                    low,high=planned[:2]
                lo,hi=int(row['begin'],16),int(row['end_exclusive'],16)
                assert low<=lo<hi<=high and (hi==high if reverse else lo==low) and row['device_steps']==hi-lo
                if a.random_window:assert (lo,hi)==(low,high)
                sequence.append(dict(begin=lo,end_exclusive=hi,reverse=reverse,overflow=row['overflow']))
                attempts+=row['device_steps']
                if row['overflow']:
                    assert not row['verified_steps'] and not row['matches'] and row['candidate_count']>capacity;overflows+=1
                    if a.random_window:limit=min(capacity//len(tags),(hi-lo)//2)
                else:
                    assert row['verified_steps']==row['device_steps']
                    observed=[(int(m['ordinal'],16),int(m['scalar'],16),m['minikey'],1 if m['encoding']=='compressed' else 2,m['hash160'],m['target']) for m in row['matches']]
                    wanted=sorted((v for v in expected if lo<=v[0]<hi),key=lambda v:(-v[0] if reverse else v[0],v[-1]))
                    assert observed==wanted,(name,lo,hi,reverse)
                    actual+=observed
                    if a.random_window:
                        model.accept()
                        if row['candidate_count']<=capacity//2:limit=min(batch,2*limit)
                    else:
                        rest=(low,lo) if reverse else (hi,high)
                        remaining[chosen:chosen+1]=[rest] if rest[0]<rest[1] else []
                    phase+=1
            summary=records[-1]
            if a.random_window:
                assert model.plan(batch,limit) is None
                assert int(start['ordinal_seed'],16)==int(summary['ordinal_seed'],16)==(seed or 0)
                assert start['ordinal_window']==summary['ordinal_window']==window
            assert (a.random_window or not remaining) and sorted(actual)==expected,(name,len(actual),len(expected))
            assert start['ordinal_order']==summary['ordinal_order']==order
            assert summary['complete'] and not summary['durable_coverage'] and summary['coordinate_space']=='minikey-ordinal-v1'
            assert int(summary['verified_steps'],16)==count and int(summary['device_steps'],16)==attempts
            assert int(summary['matches'],16)==len(expected) and summary['overflow_replays']==overflows
            if name.startswith('overflow'):assert overflows>0
            report['cases'].append(dict(name=name,length=length,device=device,count=count,admitted=len(admitted),summary=summary,**(dict(sequence=sequence) if a.both_ends or a.dance or a.random_window else {})))
report['passed']=True;report['binary_sha256']=hashlib.sha256(a.binary.read_bytes()).hexdigest()
a.report.write_text(json.dumps(report,indent=2)+'\n');print(f"Minikeys: {len(report['cases'])} independent searches, {report['rejections']} rejections, {report['inspections']} inspections")
