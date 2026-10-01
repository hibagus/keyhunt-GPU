#!/usr/bin/env python3
"""Independent integer tile bounds and reconstruction, with exhaustive tiny walks."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
from model import N
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();cases=[];rng=random.Random(0xC23B)
def add(begin,end,m,giants,reverse):
    expected='invalid'
    if 1<=begin<end<=N and 1<=m<(1<<64) and 1<=giants<=1048576 and reverse in (0,1):
        width=min(end-begin,m*giants)
        lo,hi=(end-width,end) if reverse else (begin,begin+width)
        expected=f'0x{lo:064x} 0x{hi:064x} {(width+m-1)//m} {(width-1)%m+1} 0x{hi-1:064x}'
    cases.append((f'{begin:x} {end:x} {m} {giants} {reverse}',expected))
for length in range(1,40):
    for m in (1,3,7,17):
        for giants in (1,2,5):
            for reverse in (0,1):
                # The complete independent walk tests every short final tile.
                low,high=101,101+length
                while low<high:
                    add(low,high,m,giants,reverse)
                    width=min(high-low,m*giants)
                    if reverse:high-=width
                    else:low+=width
for _ in range(512):
    begin=rng.randrange(1,N-1);end=rng.randrange(begin+1,N+1)
    for reverse in (0,1):add(begin,end,rng.randrange(1,1<<64),rng.randrange(1,1048577),reverse)
for begin,end in ((1,N),(N-1,N),(N-100,N),((1<<200)-9,(1<<200)+77)):
    for m in (1,17,(1<<64)-1):
        for reverse in (0,1):add(begin,end,m,1048576,reverse)
for row in ((0,2,1,1,0),(1,1,1,1,1),(1,N+1,1,1,1),(1,2,0,1,1),(1,2,1,0,1),(1,2,1,1048577,1),(1,2,1,1,2)):add(*row)
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=120)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
