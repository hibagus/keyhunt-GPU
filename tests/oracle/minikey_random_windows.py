#!/usr/bin/env python3
"""Compare exact sequences, retry behavior and bounded ownership independently."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from minikey_random_window import RandomWindow
p=argparse.ArgumentParser()
for key in ('binary','report'):p.add_argument('--'+key,type=Path,required=True)
a=p.parse_args();cases=[];rng=random.Random(0xC23A11);max_active=max_extra=rejections=retries=0

def add(length,gaps,attempts,seed=0,window=64,limit=1024,order='random-window'):
    global max_active,max_extra,rejections,retries
    row=' '.join(map(str,[length,f'{seed:x}',window,order,limit,len(attempts),*(v for w,b,ok in attempts for v in (f'{w:x}',b,int(ok))),len(gaps),*(f'{v:x}' for pair in gaps for v in pair)]))
    expected='invalid'
    try:
        if length not in (22,30) or order!='random-window' or any(not 1<=lo<hi<=58**(length-1)+1 for lo,hi in gaps) or any(gaps[i-1][1]>gaps[i][0] for i in range(1,len(gaps))):raise ValueError()
        model=RandomWindow(gaps,seed,window);records=[];active={};accepted=[]
        for turn in range(limit):
            span,steps,ok=attempts[turn%len(attempts)];item=model.plan(span,steps)
            if item is None:break
            lo,hi,left,right,first,last,reverse=item;work=(left,right)
            if first:assert work not in active;active[work]=right-left
            assert work in active and left<=lo<hi<=right and not reverse
            assert bool(last)==(active[work]==hi-lo)
            max_active=max(max_active,len(active));assert len(active)<=window
            if ok:
                active[work]-=hi-lo
                if last:del active[work]
                model.accept();accepted.append((lo,hi));merged=[]
                for start,end in sorted(accepted):
                    if merged and merged[-1][1]==start:merged[-1]=(merged[-1][0],end)
                    else:assert not merged or merged[-1][1]<start;merged.append((start,end))
                max_extra=max(max_extra,len(merged)-len(gaps));assert len(merged)<=len(gaps)+window
            else:retries+=1
            records.append(':'.join([*(f'0x{v:064x}' for v in item[:4]),*(str(v) for v in item[4:])]))
        rejections+=model.rejections;expected='ok'+(' '+' '.join(records) if records else '')
    except (ValueError,OverflowError):pass
    cases.append((row,expected))

for length in (22,30):
 for size in range(1,25):
  for window in (1,2,3,8,64):
   for attempts in ([(1,1,True)],[(100,7,True)],[(1,7,False),(100,2,True),(2,19,True)]):
    add(length,[(101,101+size)],attempts,rng.randrange(2**256),window)
 for window in (1,2,3,4,64,256):
  for seed in (0,1,42,2**200,2**256-1):
   for attempts in ([(1,1,True)],[(2,1,True)],[(2**256-1,3,True)],[(100,7,False),(2,1,False),(77,2,True),(1,17,True)]):
    add(length,[(1,7),(8,300),(320,660)],attempts,seed,window)
 for _ in range(96):
  cursor=rng.choice((1,2**96,58**(length-1)-10000));gaps=[]
  for _ in range(rng.randrange(1,6)):
   size=rng.randrange(1,40);gaps.append((cursor,cursor+size));cursor+=size+rng.randrange(0,9)
  add(length,gaps,[(rng.randrange(1,128),rng.randrange(1,32),i%3!=0) for i in range(5)],rng.randrange(2**256),rng.randrange(1,257))
 for steps in (1,1048576,2**64-1):
  for window in (1,64,256):add(length,[(1,58**(length-1)+1)],[(2**256-1,steps,True),(1,7,False),(23,13,True)],2**256-1,window,32)
 add(length,[],[(1,1,True)])
for length,gaps,attempts,seed,window,order in [
 (21,[(1,2)],[(1,1,True)],0,64,'random-window'),(22,[(0,2)],[(1,1,True)],0,64,'random-window'),
 (22,[(1,3),(2,4)],[(1,1,True)],0,64,'random-window'),(22,[(4,5),(1,2)],[(1,1,True)],0,64,'random-window'),
 (22,[(1,58**21+2)],[(1,1,True)],0,64,'random-window'),(22,[(1,2)],[(1,1,True)],2**256,64,'random-window'),
 *[(22,[(1,2)],[(1,1,True)],0,w,'random-window') for w in (0,257)],
 *[(22,gaps,attempts,0,64,'random-window') for gaps in ([],[(1,2)]) for attempts in ([(0,1,True)],[(1,0,True)])],
 *[(22,[(1,2)],[(1,1,True)],0,64,order) for order in ('forward','reverse','both-ends','dance','random')]]:
 add(length,gaps,attempts,seed,window,order=order)
result=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,e in cases)+'\n',text=True,capture_output=True,timeout=180)
assert result.returncode==0 and not result.stderr,result.stderr
actual=result.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),max_active_units=max_active,max_extra_fragments=max_extra,rejected_draws=rejections,unaccepted_attempts=retries,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:3])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
