#!/usr/bin/env python3
"""Independent integer/candidate sequence oracle for all ordinal orders."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from minikey import text
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();cases=[];rng=random.Random(0xC23A)
def add(length,lo,hi,work,batch,order,limit=1024):
    row=f'{length} {lo:x} {hi:x} {work} {batch} {order} {limit}';expected='invalid'
    if length in (22,30) and 1<=lo<hi<=58**(length-1)+1 and work>0 and batch>0 and order in ('forward','reverse','both-ends'):
        reverse=order=='reverse';cursor=hi if reverse else lo;work_left=0;items=[]
        # Independent ownership model: keep immutable reservations and global
        # uncovered endpoints. It does not use the C++ planner's mutable map.
        if order=='both-ends':
            low,high=lo,hi;owners=[]
            for index in range(limit):
                if low==high:break
                reverse=index%2==1;at=high-1 if reverse else low
                owner=next((v for v in owners if v[0]<=at<v[1]),None)
                if owner is None:
                    if reverse:
                        floor=max([low]+[right for left,right in owners if low<right<=at])
                        owner=(max(floor,high-work),high)
                    else:
                        ceiling=min([high]+[left for left,right in owners if at<left<high])
                        owner=(low,min(ceiling,low+work))
                    owners.append(owner)
                left,right=(max(low,owner[0],high-batch),high) if reverse else (low,min(high,owner[1],low+batch))
                first,last=(right-1,left) if reverse else (left,right-1)
                items.append(':'.join([*(f'0x{n:064x}' for n in (left,right,first,last)),text(first,length),text(last,length)]))
                if reverse:high=left
                else:low=right
            expected='ok'+(' '+' '.join(items) if items else '')
            cases.append((row,expected));return
        for _ in range(limit):
            remaining=cursor-lo if reverse else hi-cursor
            if not remaining:break
            if not work_left:work_left=min(work,remaining)
            count=min(batch,work_left);left,right=(cursor-count,cursor) if reverse else (cursor,cursor+count)
            first,last=(right-1,left) if reverse else (left,right-1)
            items.append(':'.join([*(f'0x{n:064x}' for n in (left,right,first,last)),text(first,length),text(last,length)]))
            cursor=left if reverse else right;work_left-=count
        expected='ok'+(' '+' '.join(items) if items else '')
    cases.append((row,expected))
for length in (22,30):
 for order in ('forward','reverse','both-ends'):
    end=58**(length-1)+1
    for size in range(1,25):
      for work in (1,7,100):
       for batch in (1,3,16):add(length,101,101+size,work,batch,order)
    for at in [1,end-129,*[58**k-3 for k in (1,5,length-2)],*[2**k-3 for k in (32,64,96,128,160) if 2**k<end]]:
      for work,batch in ((17,7),(1000,129),(3,100)):add(length,at,at+129,work,batch,order)
    for _ in range(64):
      lo=rng.randrange(1,end-128);add(length,lo,lo+rng.randrange(1,128),rng.randrange(1,128),rng.randrange(1,128),order)
    for work,batch in ((2**64-1,2**64-1),(2**64-1,1048576),(1048576,1)):
      add(length,1,end,work,batch,order,16)
    add(length,end-1,end,1,1,order)
for args in [(26,1,2,1,1,'reverse'),(22,0,2,1,1,'reverse'),(22,1,58**21+2,1,1,'reverse'),
             (30,3,2,1,1,'reverse'),(30,1,2,0,1,'reverse'),(30,1,2,1,0,'reverse'),(30,1,2,1,1,'random')]:add(*args)
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,e in cases)+'\n',text=True,capture_output=True,timeout=120)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:3])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
