#!/usr/bin/env python3
"""Compare full submitted sequences, mappings and ownership, including retries."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from model import N
from scalar_batches import Planner
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
rng=random.Random(0xc23b);cases=[];max_owners=0;retries=0

def add(lo,hi,step,reverse,orbit,order,works,batches,accepts,gaps=None,limit=2048):
    global max_owners,retries
    n=(hi-lo-1)//step+1;mapped=step!=1 or reverse or orbit
    root=(1,n*(6 if orbit else 1)+1) if mapped else (lo,hi)
    gaps=[root] if gaps is None else gaps
    row=' '.join([f'{lo:x}',f'{hi:x}',f'{step:x}',str(int(reverse)),str(int(orbit)),order,
                  ','.join(f'{w:x}' for w in works),','.join(map(str,batches)),','.join(map(str,accepts)),str(limit),
                  ','.join(f'{l:x}:{h:x}' for l,h in gaps) or '-'])
    model=Planner(gaps,order,n if orbit else None);out=[]
    def scalar(i):
        if not mapped:return i
        variant,offset=divmod(i-1,n)
        seed=lo+(n-1-offset if reverse else offset)*step
        return seed*pow(L,variant//2,N)*(-1 if variant%2 else 1)%N if orbit else seed
    for i in range(limit):
        selected=model.plan(works[i%len(works)],batches[i%len(batches)])
        if selected is None:break
        l,h,wl,wh,start,finish=selected
        out.append(':'.join([*(f'0x{v:064x}' for v in (l,h,wl,wh)),str(start),str(finish),
                             f'0x{scalar(l):064x}',f'0x{scalar(h-1):064x}',str((l-1)//n if orbit else 0)]))
        max_owners=max(max_owners,len(model.owners));assert len(model.owners)<=2
        if accepts[i%len(accepts)]:model.accept()
        else:retries+=1
    cases.append((row,'ok'+(' '+' '.join(out) if out else '')))
for order in ('forward','both-ends'):
 for reverse,orbit,step in [(False,False,1),(False,False,7),(True,False,1),(True,False,7),(False,True,7),(True,True,1)]:
  for count in range(1,30):
   for works,batches,accepts in [([5],[3],[1]),([1,31,7],[19,1,3],[0,0,1,1]),([100],[11,1,17],[1,0,1])]:
    add(101,101+count*step,step,reverse,orbit,order,works,batches,accepts)
  for _ in range(30):
    lo=rng.randrange(1,N//8);count=rng.randrange(10,150);hi=lo+count*step
    mapped=step!=1 or reverse or orbit;start,end=(1,count*(6 if orbit else 1)+1) if mapped else (lo,hi)
    cuts=sorted(rng.sample(range(start,end+1),8));gaps=list(zip(cuts[::2],cuts[1::2]))
    add(lo,hi,step,reverse,orbit,order,[3,1<<100,31],[17,2,1,33],[0,1,1],gaps)
  add(1,N//6 if orbit else N,step,reverse,orbit,order,[1<<200],[(1<<64)-1,1048576,1],[0,1,1],limit=30)
  add(N-100,N,step,reverse,orbit,order,[17],[11],[1])
  add(101,138,step,reverse,orbit,order,[7],[3],[1],[])
# Deliberate malformed inputs: enum, zero limits, overlapping/unsorted/outside gaps.
base='65 100 1 0 0 both-ends 5 3 1 10 '
for suffix in ('64:70','65:101','70:80,65:70','65:80,70:90'):
    cases.append((base+suffix,'invalid'))
for row in ('65 100 1 0 0 random 5 3 1 10 65:100','65 100 1 0 0 forward 0 3 1 10 65:100','65 100 1 0 0 forward 5 0 1 10 65:100'):
    cases.append((row,'invalid'))
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=150)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),retries=retries,max_owners=max_owners,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:2])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
