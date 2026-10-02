#!/usr/bin/env python3
"""Independent shuffle sequences, scalar mappings, ownership and exact coverage."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from model import N
from scalar_random_window import RandomWindow
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();rng=random.Random(0xC23AC);cases=[];retries=draw_rejections=max_active=max_extra=0

def add(lo,hi,step,reverse,orbit,works,batches,accepts,seed=0,window=64,gaps=None,limit=1024):
 global retries,draw_rejections,max_active,max_extra
 n=(hi-lo-1)//step+1;mapped=step!=1 or reverse or orbit
 root=(1,n*(6 if orbit else 1)+1) if mapped else (lo,hi);gaps=[root] if gaps is None else gaps
 row=' '.join([f'{lo:x}',f'{hi:x}',f'{step:x}',str(int(reverse)),str(int(orbit)),'random-window',
               ','.join(f'{w:x}' for w in works),','.join(map(str,batches)),','.join(map(str,accepts)),str(limit),
               ','.join(f'{l:x}:{h:x}' for l,h in gaps) or '-',f'{seed:x}',str(window)])
 model=RandomWindow(gaps,seed,window,n if orbit else None);out=[];active={};covered=[]
 def scalar(i):
  if not mapped:return i
  variant,offset=divmod(i-1,n);seed_scalar=lo+(n-1-offset if reverse else offset)*step
  return seed_scalar*pow(L,variant//2,N)*(-1 if variant%2 else 1)%N if orbit else seed_scalar
 for i in range(limit):
  item=model.plan(works[i%len(works)],batches[i%len(batches)])
  if item is None:assert not active;break
  l,h,wl,wh,start,finish=item;owner=(wl,wh)
  if start:assert owner not in active;active[owner]=wh-wl
  assert owner in active and wl<=l<h<=wh and wh-wl<2**64
  assert bool(finish)==(active[owner]==h-l)
  if orbit:assert (l-1)//n==(h-2)//n
  max_active=max(max_active,len(active));assert len(active)<=window
  out.append(':'.join([*(f'0x{v:064x}' for v in (l,h,wl,wh)),str(start),str(finish),f'0x{scalar(l):064x}',f'0x{scalar(h-1):064x}',str((l-1)//n if orbit else 0)]))
  if accepts[i%len(accepts)]:
   model.accept();active[owner]-=h-l
   if finish:del active[owner]
   covered.append((l,h));merged=[]
   for begin,end in sorted(covered):
    if merged and merged[-1][1]==begin:merged[-1]=(merged[-1][0],end)
    else:assert not merged or merged[-1][1]<begin;merged.append((begin,end))
   max_extra=max(max_extra,len(merged)-len(gaps));assert len(merged)<=len(gaps)+window
  else:retries+=1
 draw_rejections+=model.rejections;cases.append((row,'ok'+(' '+' '.join(out) if out else '')))

for reverse,orbit,step in [(False,False,1),(True,False,1),(False,False,7),(True,False,7),(False,True,1),(True,True,7)]:
 for size in range(1,25):
  for window in (1,2,3,8,64):
   for work,batch,ok in [([1],[1],[1]),([100],[7],[1]),([1,100,2],[7,2,19],[0,1,1])]:
    add(101,101+size*step,step,reverse,orbit,work,batch,ok,rng.randrange(2**256),window)
 for window in (1,3,64,256):
  for seed in (0,42,2**200,2**256-1):
   add(101,101+660*step,step,reverse,orbit,[2,100,2**256-1],[7,1,3],[0,1,1],seed,window,limit=2048)
 for _ in range(32):
  lo=rng.randrange(1,N//8);size=rng.randrange(10,100);mapped=step!=1 or reverse or orbit
  start,end=(1,size*(6 if orbit else 1)+1) if mapped else (lo,lo+size)
  cuts=sorted(rng.sample(range(start,end+1),8))
  add(lo,lo+size*step,step,reverse,orbit,[3,2**256-1,31],[17,2,1,33],[0,1,1],rng.randrange(2**256),rng.randrange(1,257),list(zip(cuts[::2],cuts[1::2])))
 for batch in (1,1048576,2**64-1):
  add(1,N//6 if orbit else N,step,reverse,orbit,[2**256-1],[batch,7,13],[1,0,1],2**256-1,256,limit=32)
 add(N-100,N,step,reverse,orbit,[17],[11],[1],42,3)
 add(101,138,step,reverse,orbit,[1],[1],[1],gaps=[])
# Large windows exercise interleaved two-tile owners and one owner per tile.
for work in (1,2):add(1,1025,1,False,False,[work],[1],[1],42,256,limit=2048)
base='65 100 1 0 0 random-window 5 3 1 10 '
for gaps in ('64:70','65:101','70:80,65:70','65:80,70:90'):
 cases.append((base+gaps+' 0 64','invalid'))
for settings in ('0 0','0 257','0 -1','0 +2','0 2x','-1 64','junk 64',f'{2**256:x} 64'):
 cases.append((base+'65:100 '+settings,'invalid'))
for order in ('forward','both-ends','dance','random'):
 cases.append((base.replace('random-window',order)+'65:100 0 64','invalid'))
for gap in ('-','65:100'):
 for sizes in ('0 3','5 0'):
  cases.append((base.replace('5 3',sizes)+gap+' 0 64','invalid'))
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=180)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),unaccepted_attempts=retries,rejected_draws=draw_rejections,max_active_units=max_active,max_extra_fragments=max_extra,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:2])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
