#!/usr/bin/env python3
"""Pinned-public-key parity and durable direction switches for BSGS tile traversal."""
import argparse,hashlib,json,select,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from model import N
from bsgs_random_window import RandomWindow
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--backend',choices=('hip','cuda'),default='hip');p.add_argument('--hardware',action='store_true')
p.add_argument('--suite',choices=('search','recovery'),required=True)
choice=p.add_mutually_exclusive_group()
choice.add_argument('--both-ends',action='store_true')
choice.add_argument('--dance',action='store_true')
choice.add_argument('--random-window',action='store_true')
a=p.parse_args();binary=str(a.binary.resolve())
selected='random-window' if a.random_window else 'dance' if a.dance else 'both-ends' if a.both_ends else None
orders=(selected,) if selected else ('forward','reverse')
switches=('forward','reverse','both-ends','dance','random-window') if a.random_window else ('forward','reverse','both-ends','dance') if a.dance else ('forward','reverse','both-ends') if a.both_ends else ('forward','reverse')
report=dict(random_window=a.random_window,passed=False,both_ends=a.both_ends,dance=a.dance,suite=a.suite,backend=a.backend,hardware=a.hardware,cases=[],oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest())
def tuning(order,seed=42,window=4):
    return ['--tile-seed',f'{seed:x}','--tile-window',window] if order=='random-window' else []
def run(words,ok=True):
    result=subprocess.run([binary,*map(str,words)],capture_output=True,text=True,timeout=120)
    assert (result.returncode==0)==ok,(words,result.stdout[-2000:],result.stderr)
    return [json.loads(line) for line in result.stdout.splitlines()] if ok else result
with tempfile.TemporaryDirectory(prefix='kh-bsgs-reverse-',dir='/var/tmp') as temporary:
    root=Path(temporary);table=root/'babies.khb';run(['bsgs-table','build','--m',17,'--output',table])
    def fixture(label,begin,end,dense=False,miss=False):
        seeds=[1] if miss else list(range(begin,end)) if dense else sorted({begin,begin+1 if end-begin>1 else begin,begin+(end-begin)//2,end-2 if end-begin>1 else begin,end-1,*([begin-1] if begin>1 else []),*([end] if end<N else [])})
        public=oracle_run(a.oracle,[f'pub {k:064x}' for k in seeds]);file=root/(label+'.txt')
        # Compressed/uncompressed duplicates must normalize identically in either order.
        file.write_text('\n'.join(public+[('03' if int(pub[-2:],16)&1 else '02')+pub[2:66] for pub in reversed(public)])+'\n')
        return file,{(k,pub) for k,pub in zip(seeds,public) if begin<=k<end}
    if a.suite=='search':
        inventory=run(['devices','--backend',a.backend])[0] if a.hardware else None;report['inventory']=inventory
        fixtures=[('low',1,82,False,False),('wide',(1<<200)-3,(1<<200)+70,False,False),
                  ('order',N-73,N,False,False),('single',N-1,N,False,False),('miss',101,144,False,True),('dense',101,147,True,False)]
        def exercise(label,begin,end,dense,miss,order,group,device=0,giants=2,seed=42,window=4,defaults=False):
            file,wanted=fixture(label,begin,end,dense,miss)
            words=['bsgs','--backend',a.backend,'--device',device,'--range',f'{begin:x}:{end:x}','--targets',file,'--table',table,
                   '--tile-order',order,'--group-size',group,'--giant-batch',giants,'--target-batch',1 if giants==1048576 else 8,'--candidate-capacity',1 if dense else 1024]
            if not defaults:words+=tuning(order,seed,window)
            if not a.hardware:assert 'not built' in run(words,False).stderr;return
            rows=run(words);summary=rows[-1];turn=0;found=[]
            random=RandomWindow([(begin,end)],17,giants,seed,window) if order=='random-window' else None
            planned=random.next(17*giants) if random else None;sequence=[]
            if random:
                for value in (rows[0],summary):assert int(value['tile_seed'],16)==seed and value['tile_window']==window
            pivot=begin+(end-begin)//2
            missing=[(begin,pivot),(pivot,end)] if order=='dance' and begin<pivot<end else [(begin,end)]
            assert rows[0]['tile_order']==summary['tile_order']==order
            for row in rows[1:-1]:
                if row['type']=='batch':
                    if random:assert (int(row['begin'],16),int(row['end_exclusive'],16))==planned[:2]
                    if row['overflow']:assert row['verified_steps']==0 and not row['matches']
                    else:found.extend((int(m['scalar'],16),m['public_key']) for m in row['matches'])
                if row['type']=='tile':
                    lo,hi=int(row['begin'],16),int(row['end_exclusive'],16)
                    if random:
                        assert (lo,hi)==planned[:2];sequence.append([lo,hi]);planned=random.next(17*giants);turn+=1;continue
                    from_high=order=='reverse' or (order=='both-ends' and turn%2==1) or (order=='dance' and turn%3==1)
                    index=len(missing)-1 if from_high else 0
                    if order=='dance' and turn%3==2:index=next((i for i,(x,y) in enumerate(missing) if x>=pivot),0)
                    low,high=missing[index];width=min(17*giants,high-low)
                    assert (lo,hi)==((high-width,high) if from_high else (low,low+width))
                    if (lo,hi)==(low,high):missing.pop(index)
                    else:missing[index]=(low,lo) if from_high else (hi,high)
                    turn+=1
            assert planned is None if random else not missing
            assert len(found)==len(wanted) and set(found)==wanted
            assert summary['complete'] and int(summary['verified_scalars'],16)==end-begin
            if dense:assert summary['overflow_replays']>0
            report['cases'].append(dict(name=label,order=order,group=group,device=device,relations=len(wanted),summary=summary,**(dict(seed=seed,window=window,sequence=sequence) if random else {})))
        for order in orders:
            for group in (1,8):
                for case in fixtures:exercise(*case,order,group)
        if a.hardware:
            for device in range(len(inventory['devices'])):exercise('ordinal',101,179,False,False,selected or 'reverse',8,device)
            for group in (1,8):exercise('maximum-tail',1<<128,(1<<128)+17*1048576+1,False,False,selected or 'reverse',group,giants=1048576)
        if a.random_window:
            for window,seed in ((1,0),(2,1),(3,42),(64,0),(256,2**256-1)):
                for group in (1,8):exercise('window-'+str(window),101,101+34*(window+1)+3,False,False,'random-window',group,seed=seed,window=window)
            exercise('defaults',101,300,False,False,'random-window',8,seed=0,window=64,defaults=True)
            base=['bsgs','--backend',a.backend,'--range','1:2','--targets',root/'unused','--table',table]
            for order in ('forward','reverse','both-ends','dance'):
                assert 'require tile-order random-window' in run(base+['--tile-order',order,'--tile-seed','0'],False).stderr
            for words in (['--tile-window','0'],['--tile-window','257'],['--tile-window','-1'],['--tile-window','x'],['--tile-seed','g'],['--tile-seed','1'+'0'*64],['--tile-seed','']):
                error=run(base+['--tile-order','random-window']+words,False);assert not error.stdout
        for value in ('random','backward',''):
            rejected=run(['bsgs','--backend',a.backend,'--range','1:2','--targets',root/'unused','--table',table,'--tile-order',value],False)
            assert 'tile-order must be' in rejected.stderr and not rejected.stdout
    else:
        state=root/'state'
        def call(family,action,*words,ok=True):return run([family,action,'--state-dir',state,*words],ok)
        def prepare(label,begin,end,dense=False):
            file,wanted=fixture(label,begin,end,dense)
            project=call('state','project-create','--name',label)[0]['project']
            words=['--project',project,'--mode','bsgs','--range',f'{begin:x}:{end:x}','--block-width',f'{end-begin:x}','--targets',file,'--table',table]
            job=call('checkpoint','create',*words)[0]
            assert call('checkpoint','create',*words)[0]==job
            assert not call('checkpoint','create',*words,'--tile-order','reverse',ok=False).stdout
            if a.random_window:
                for extra in (['--tile-seed','0'],['--tile-window','4']):assert not call('checkpoint','create',*words,*extra,ok=False).stdout
            scope=['--project',project,'--job',job['job']]
            grant=call('state','claim',*scope,'--owner','test','--request','claim')[0]['assignments'][0]['grant']
            return scope,['--backend',a.backend,'--grant',grant,'--targets',file,'--table',table],wanted
        def verify(scope,wanted):
            rows=call('checkpoint','results',*scope,'--limit','1000')[0]['results']
            assert len(rows)==len(wanted) and {(int(r['scalar'],16),r['target_bytes']) for r in rows}==wanted
            assert call('state','block',*scope,'--block',0)[0]['state']=='finished';call('state','check')
        # Supplying even the default tile order to a scalar job is an error,
        # caught before an executor can publish any coverage.
        if a.hardware:
            xfile=root/'xpoint.txt';xfile.write_text(oracle_run(a.oracle,['pub '+f'{101:064x}'])[0][2:66]+'\n')
            project=call('state','project-create','--name','wrong-mode')[0]['project']
            job=call('checkpoint','create','--project',project,'--mode','xpoint','--range','65:67','--block-width','2','--targets',xfile)[0]
            scope=['--project',project,'--job',job['job']]
            grant=call('state','claim',*scope,'--owner','test','--request','wrong-mode')[0]['assignments'][0]['grant']
            for order in orders:
                error=call('checkpoint','run','--backend',a.backend,'--grant',grant,'--targets',xfile,'--tile-order',order,ok=False)
                assert 'tile-order applies only to BSGS' in error.stderr and not error.stdout
            assert not call('state','block',*scope,'--block',0)[0]['covered']
        for order in orders:
            for group in (1,8):
                scope,words,wanted=prepare(order+str(group),101,147,True)
                if not a.hardware:assert 'not built' in call('checkpoint','run',*words,'--tile-order',order,ok=False).stderr;continue
                result=call('checkpoint','run',*words,'--tile-order',order,*tuning(order),'--group-size',group,'--giant-batch',2,'--target-batch',8,'--candidate-capacity',1)[-1]
                assert result['complete'] and result['tile_order']==order and int(result['computed_scalars'],16)==46 and result['overflow_replays']>0
                if order=='random-window':assert int(result['tile_seed'],16)==42 and result['tile_window']==4
                verify(scope,wanted)
                retry=call('checkpoint','run',*words,'--tile-order','forward' if order=='reverse' else 'reverse')[-1]
                assert retry['complete'] and retry['batches']==0 and int(retry['resumed_scalars'],16)==46
                report['cases'].append(dict(name='dense',order=order,group=group,summary=result,retry=retry))
        if a.hardware:
            for first in switches:
                for resume in switches:
                    if selected and selected not in (first,resume):continue
                    begin=(1<<128)+3;end=begin+1048577
                    scope,words,wanted=prepare('killed-'+first+'-'+resume,begin,end)
                    command=[binary,'checkpoint','run','--state-dir',state,*words,'--tile-order',first,'--giant-batch',1,'--target-batch',1,'--group-size',1]
                    command+=tuning(first)
                    child=subprocess.Popen(list(map(str,command)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    try:
                        assert select.select([child.stdout],[],[],30)[0],'no durable acknowledgment'
                        notice=json.loads(child.stdout.readline());assert notice['durable_results'] and (first=='random-window' or not notice['durable_coverage'])
                        assert 'already held' in call('checkpoint','run',*words,'--tile-order',resume,ok=False).stderr
                        child.kill();child.communicate(timeout=30)
                    finally:
                        if child.poll() is None:child.kill();child.communicate(timeout=30)
                    saved=call('state','block',*scope,'--block',0)[0]
                    covered=sum(int(v['end_exclusive'],16)-int(v['begin'],16) for v in saved['covered'])
                    assert covered<end-begin
                    result=call('checkpoint','run',*words,'--tile-order',resume,*tuning(resume,seed=2**256-1,window=3),'--giant-batch',65536,'--target-batch',8,'--group-size',8)[-1]
                    assert result['complete'] and int(result['resumed_scalars'],16)==covered and covered+int(result['computed_scalars'],16)==end-begin
                    if resume=='random-window':assert int(result['tile_seed'],16)==2**256-1 and result['tile_window']==3
                    verify(scope,wanted);report['cases'].append(dict(name='kill-restart',first=first,resume=resume,partial_ack=notice,retained=saved,summary=result))
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print('PASS BSGS tile '+a.suite)
