#!/usr/bin/env python3
"""Check bounded seeded tile sequences and accounting against Python integers."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from model import N
from bsgs_random_window import RandomWindow
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();cases=[];rng=random.Random(0xC23B55);max_active=0;rejections=0;max_fragments=0

def add(m,giants,sizes,gaps,seed,window,limit=1024,order='random-window'):
    global max_active,rejections,max_fragments
    row=' '.join(map(str,[order,m,giants,limit,len(sizes),*(f'{v:x}' for v in sizes),len(gaps),*(f'{v:x}' for pair in gaps for v in pair),f'{seed:x}',window]))
    expected='invalid'
    try:
        if order!='random-window' or not 0<=seed<2**256 or any(not 1<=lo<hi<=N for lo,hi in gaps) or any(gaps[i-1][1]>gaps[i][0] for i in range(1,len(gaps))):raise ValueError()
        model=RandomWindow(gaps,m,giants,seed,window);records=[];active={};accepted=[]
        for index in range(limit):
            item=model.next(sizes[index%len(sizes)])
            if item is None:break
            lo,hi,left,right,first,last=item;work=(left,right)
            if first:
                assert work not in active;active[work]=right-left
            assert work in active and left<=lo<hi<=right
            active[work]-=hi-lo;assert bool(last)==(active[work]==0)
            max_active=max(max_active,len(active));assert len(active)<=window
            if last:del active[work]
            # A sorted accepted union measures fragmentation without mirroring
            # production gap deletion or its per-window ownership map.
            accepted.append((lo,hi));ordered=sorted(accepted);merged=[]
            for begin,end in ordered:
                if merged and merged[-1][1]==begin:merged[-1]=(merged[-1][0],end)
                else:
                    assert not merged or merged[-1][1]<begin
                    merged.append((begin,end))
            assert len(merged)<=len(gaps)+window
            max_fragments=max(max_fragments,len(merged)-len(gaps))
            records.append(':'.join([*(f'0x{v:064x}' for v in item[:4]),*(str(v) for v in item[4:])]))
        rejections+=model.rejections
        expected='ok'+(' '+' '.join(records) if records else '')
    except (ValueError,OverflowError):pass
    cases.append((row,expected))

for length in range(1,25):
 for m,giants in ((1,1),(3,2),(7,5)):
  for window in (1,2,3,8,64):
   for sizes in ([1],[100],[2,17,1,31]):add(m,giants,sizes,[(101,101+length)],rng.randrange(2**256),window)
for window in (1,2,3,4,64,256):
 for seed in (0,1,42,2**200,2**256-1):
  for sizes in ([1],[2],[77],[2**256-1]):add(1,1,sizes,[(1,7),(8,300),(320,660)],seed,window)
for _ in range(128):
 cursor=rng.choice((1,1<<200,N-10000));gaps=[]
 for _ in range(rng.randrange(1,6)):
  width=rng.randrange(1,50);gaps.append((cursor,cursor+width));cursor+=width+rng.randrange(0,9)
 add(rng.randrange(1,18),rng.randrange(1,8),[rng.randrange(1,128) for _ in range(4)],gaps,rng.randrange(2**256),rng.randrange(1,257))
for m in (1,17,2**64-1):
 for giants in (1,1048576):
  for window in (1,64,256):add(m,giants,[N-1,1<<100,1],[(1,N)],2**256-1,window,32)
add(17,2,[1],[],0,64)
for m,g,sizes,gaps,seed,window,order in [
 (0,1,[1],[(1,2)],0,64,'random-window'),(1,0,[1],[(1,2)],0,64,'random-window'),(1,1048577,[1],[(1,2)],0,64,'random-window'),
 (1,1,[0],[(1,2)],0,64,'random-window'),(1,1,[0],[],0,64,'random-window'),(1,1,[1],[(0,2)],0,64,'random-window'),
 (1,1,[1],[(N-1,N+1)],0,64,'random-window'),(1,1,[1],[(1,3),(2,4)],0,64,'random-window'),
 (1,1,[1],[(3,4),(1,2)],0,64,'random-window'),(1,1,[1],[(1,2)],2**256,64,'random-window'),
 (1,1,[1],[(1,2)],0,0,'random-window'),(1,1,[1],[(1,2)],0,257,'random-window'),
 *[(1,1,[1],[(1,2)],0,64,order) for order in ('forward','reverse','both-ends','dance','random')]]:add(m,g,sizes,gaps,seed,window,order=order)
result=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,e in cases)+'\n',text=True,capture_output=True,timeout=150)
assert result.returncode==0 and not result.stderr,result.stderr
actual=result.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),max_active_units=max_active,max_extra_fragments=max_fragments,rejected_draws=rejections,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:3])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
