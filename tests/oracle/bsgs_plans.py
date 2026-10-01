#!/usr/bin/env python3
"""Independent missing-set and work-ownership model for exact BSGS tile sequences."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from model import N
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument("--dance",action="store_true")
a=p.parse_args();cases=[];rng=random.Random(0xB07E);max_active=0;max_extra_gaps=0
def add(order,m,giants,sizes,gaps,limit=1024):
    global max_active,max_extra_gaps
    row=' '.join(map(str,[order,m,giants,limit,len(sizes),*(f'{n:x}' for n in sizes),len(gaps),*(f'{n:x}' for pair in gaps for n in pair)]))
    expected='invalid'
    if order in ('forward','reverse','both-ends','dance') and 0<m<1<<64 and 0<giants<=1048576 and all(1<=lo<hi<=N for lo,hi in gaps) and all(gaps[i-1][1]<=gaps[i][0] for i in range(1,len(gaps))):
        missing=list(gaps);active=[];records=[];valid=True
        pivot=(gaps[0][0]+gaps[-1][1])//2 if gaps else 0
        if order=='dance':
            # A static partition is independent of work ownership and of the
            # production interval index. No later step creates an interior hole.
            missing=[part for lo,hi in gaps for part in ([(lo,pivot),(pivot,hi)] if lo<pivot<hi else [(lo,hi)])]
        for i in range(limit):
            span=sizes[i%len(sizes)]
            if span==0:valid=False;break
            if not missing:break
            high=order=='reverse' or (order=='both-ends' and i%2==1) or (order=='dance' and i%3==1)
            index=len(missing)-1 if high else 0
            if order=='dance' and i%3==2:index=next((j for j,(lo,hi) in enumerate(missing) if lo>=pivot),0)
            lo,hi=missing[index];point=hi-1 if high else lo
            max_extra_gaps=max(max_extra_gaps,len(missing)-len(gaps))
            assert len(missing)<=len(gaps)+(order=='dance')
            owners=[work for work in active if work[0]<=point<work[1]];assert len(owners)<=1
            started=not owners
            if started:
                # Reserve only currently unowned space; an unfinished unit at
                # the opposite end is still missing, but already has an owner.
                free_lo=max([lo]+[w[1] for w in active if w[1]<=point])
                free_hi=min([hi]+[w[0] for w in active if w[0]>point])
                work=(max(free_lo,hi-span),hi) if high else (lo,min(free_hi,lo+span));active.append(work)
            else:work=owners[0]
            max_active=max(max_active,len(active))
            assert len(active)<=(3 if order=="dance" else 2)
            width=min(m*giants,hi-lo,(hi-work[0]) if high else (work[1]-lo))
            tile=(hi-width,hi) if high else (lo,lo+width)
            assert lo<=tile[0]<tile[1]<=hi
            if tile==(lo,hi):missing.pop(index)
            else:missing[index]=(lo,tile[0]) if high else (tile[1],hi)
            finished=not any(max(x,work[0])<min(y,work[1]) for x,y in missing)
            if finished:active.remove(work)
            assert len(active)<=(3 if order=="dance" else 2)
            records.append(':'.join([*(f'0x{v:064x}' for v in (*tile,*work)),str(int(started)),str(int(finished))]))
        if valid:expected='ok'+(' '+' '.join(records) if records else '')
    cases.append((row,expected))
for order in (('dance',) if a.dance else ('forward','reverse','both-ends')):
    for length in range(1,25):
        for m in (1,3,7):
            for giants in (1,2,5):
                for sizes in ([1],[m*giants],[100],[2,17,1,31]):
                    add(order,m,giants,sizes,[(101,101+length)])
    for length in range(1,14):
        for sizes in ([1],[7],[100],[2,17,1,31]):
            add(order,3,2,sizes,[(1,length+1),(length+3,2*length+4),(2*length+7,3*length+7)])
    for _ in range(128):
        origin=rng.choice([1,1<<200,N-10000]);cursor=origin;gaps=[]
        for _ in range(rng.randrange(1,6)):
            width=rng.randrange(1,50);gaps.append((cursor,cursor+width));cursor+=width+rng.randrange(0,9)
        add(order,rng.randrange(1,18),rng.randrange(1,8),[rng.randrange(1,128) for _ in range(4)],gaps)
    for m in (1,17,(1<<64)-1):
        for giants in (1,1048576):
            add(order,m,giants,[N-1,1<<100,1],[(1,N)],limit=32)
    add(order,17,2,[100],[])
for args in [('both',1,1,[1],[(1,2)]),('dancing',1,1,[1],[(1,2)]),('both-ends',0,1,[1],[(1,2)]),
             ('both-ends',1,0,[1],[(1,2)]),('both-ends',1,1048577,[1],[(1,2)]),('both-ends',1,1,[0],[(1,2)]),
             ('both-ends',1,1,[1],[(0,2)]),('both-ends',1,1,[1],[(N-1,N+1)]),('both-ends',1,1,[1],[(1,3),(2,4)]),
             ('both-ends',1,1,[1],[(3,4),(1,2)]),('both-ends',1,1,[0],[])]:add(*args)
if a.dance:
    # Keep three reservations alive, then exercise meeting fronts and pivot
    # inside a covered island / exactly on either saved-gap boundary.
    for gaps in ([(1,400)],[(1,17),(100,117)],[(1,58),(58,115)],[(1,58),(59,117)]):
        for sizes in ([80],[2,97,33,1]):add('dance',3,2,sizes,gaps)
    add('dance',1,1,[100],[(1,400)],limit=1024)
result=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=120)
assert result.returncode==0 and not result.stderr,result.stderr
actual=result.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),dance=a.dance,max_active_units=max_active,max_extra_gaps=max_extra_gaps,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:3])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
