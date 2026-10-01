#!/usr/bin/env python3
"""Independent six-member candidate sets, including repeated/overlapping orbits."""
import argparse,hashlib,json,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N
from scalar_batches import Planner
from oracle_selftest import check_source,run as oracle_run
from stride import targets,relations
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',default='hip',choices=('hip','cuda'))
p.add_argument('--batch-order',choices=('forward','both-ends'),default='forward')
p.add_argument('--order',choices=('forward','reverse'),default='forward');a=p.parse_args();binary=str(a.binary.resolve())
report=dict(batch_order=a.batch_order,passed=False,order=a.order,hardware=a.hardware,cases=[],rejections=0,oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest())
def run(words,ok=True):
 r=subprocess.run([binary,*map(str,words)],capture_output=True,text=True,timeout=180)
 assert (r.returncode==0)==ok,(words,r.stdout[-1000:],r.stderr)
 return [json.loads(line) for line in r.stdout.splitlines()] if ok else r
with tempfile.TemporaryDirectory(prefix='kh-orbit-cli-') as temporary:
 root=Path(temporary);inventory=run(['devices','--backend',a.backend])[0] if a.hardware else None;report['inventory']=inventory
 cases=[('miss',101,102,1,False),('unit',101,110,1,True),('strided',(1<<128)-11,(1<<128)-11+11*17-5,17,False),
        ('wide',1<<100,(1<<100)+3*((1<<192)+19),(1<<192)+19,False),
        ('overlap',1,N,N-2,False),('single',N-1,N,N-1,False)]
 def exercise(mode,kernel,name,begin,end,step,overflow=False,device=0):
  count=1+(end-begin-1)//step
  offsets=sorted({0,1,count-1,*[1<<bit for bit in range(20)]}) if count>1048576 else list(range(count))
  indexed=[]
  for variant in range(6):
   for offset in offsets:
    seed=begin+((count-offset-1) if a.order=='reverse' else offset)*step
    scalar=seed*pow(L,variant//2,N)*(-1 if variant%2 else 1)%N
    indexed.append((variant*count+offset+1,seed,variant,scalar))
  public=oracle_run(a.oracle,[f'pub {row[3]:064x}' for row in indexed])
  negative=oracle_run(a.oracle,['pub '+f'{1:064x}']) if name=='miss' else public
  lines,canonical=targets(mode,negative,overlap=overflow and mode=='vanity');file=root/'targets.txt';file.write_text('\n'.join(lines))
  bound=2*len({len(line) for line in lines}) if mode=='vanity' else 2 if mode in ('hash160','address') else 1
  words=[mode,'--backend',a.backend,'--range',f'{begin:x}:{end:x}','--stride',f'{step:x}','--order',a.order,
         '--endomorphism','orbit','--targets',file,'--kernel',kernel,'--device',device,'--batch-size','1048576',
         '--candidate-capacity',bound if overflow else 4096]
  maximum=1048576 if a.batch_order=='forward' or count>=1048576 else 17
  words[words.index('--batch-size')+1]=maximum
  words+=['--batch-order',a.batch_order]
  if not a.hardware:
   assert 'not built' in run(words,False).stderr;return
  rows=run(words);start,summary=rows[0],rows[-1]
  assert start['batch_order']==summary['batch_order']==a.batch_order
  assert start['kernel']==kernel and start['endomorphism']=='orbit' and int(start['seed_count'],16)==count
  assert start['coordinate_space']==summary['coordinate_space']=='scalar-orbit-index-v1'
  assert int(start['begin'],16)==1 and int(start['end_exclusive'],16)==6*count+1
  model=Planner([(1,6*count+1)],a.batch_order,count);limit=maximum;found=[];replayed=0;batches=0
  for row in rows[1:-1]:
   assert row['type']=='batch';batches+=1
   low,high=int(row['begin'],16),int(row['end_exclusive'],16)
   assert (low,high)==model.plan(maximum,limit)[:2]
   if row['overflow']:
    limit=min((bound if overflow else 4096)//bound,(high-low)//2)
    assert not row['verified_steps'] and not row['matches'];replayed+=1;continue
   low,high=int(row['begin'],16),int(row['end_exclusive'],16)
   assert row['verified_steps']==row['device_steps']==high-low
   assert (low-1)//count==(high-2)//count # no admitted batch crosses a variant
   model.accept()
   if row['candidate_count']<=(bound if overflow else 4096)//2:limit=min(maximum,2*limit)
   found.extend((int(m['candidate_index'],16),int(m['seed_scalar'],16),m['orbit_variant'],int(m['scalar'],16),m['target']) for m in row['matches'])
  wanted={(i,seed,v,k,t) for (i,seed,v,k),pub in zip(indexed,public) for t in relations(mode,pub,canonical)}
  assert len(found)==len(wanted) and set(found)==wanted,(mode,kernel,name,len(found),len(wanted))
  assert not model.gaps and summary['complete'] and int(summary['verified_steps'],16)==6*count
  if overflow:assert replayed>0
  report['cases'].append(dict(mode=mode,kernel=kernel,name=name,device=device,seeds=count,relations=len(wanted),batches=batches,summary=summary))
 for mode in (('xpoint','hash160','address','ethereum','vanity') if a.batch_order=='both-ends' else ('xpoint','hash160','ethereum','vanity')):
  for kernel in ('direct','glv','stepped'):
   for case in cases:exercise(mode,kernel,*case)
  if a.hardware and a.order=='forward':exercise(mode,'stepped','maximum-tail',1<<128,(1<<128)+1048577,1)
  base=[mode,'--backend',a.backend,'--range','1:2','--targets',root/'targets.txt']
  for value in ('lambda','expanded',''):
   assert not run(base+['--endomorphism',value],False).stdout;report['rejections']+=1
  assert not run([mode,'--backend',a.backend,'--range',f'1:{N:x}','--targets',root/'targets.txt','--endomorphism','orbit'],False).stdout
  report['rejections']+=1
 if a.hardware:
  for device in range(len(inventory['devices'])):
   exercise(('xpoint','hash160','ethereum','vanity')[device%4],'stepped','visible-device',101,119,7,False,device)
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
