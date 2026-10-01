#!/usr/bin/env python3
"""Python integers independently verify candidate counts, mapping and powers."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
n=0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
rng=random.Random(0xC2357D);cases=[]
def mapping(start,end,step,index):
    valid=1<=start<end<=n and 2<=step<n
    count=1+(end-start-1)//step if valid else 0
    wanted=f'0x{count:064x} 0x{start+(index-1)*step:064x} 0x{index:064x}' if valid and 1<=index<=count else 'invalid'
    cases.append((f'map {start:x} {end:x} {step:x} {index:x}',wanted))
for start in (1,17,n-257):
    for span in (1,2,3,17,257):
        for step in (2,3,16,256,n-1):
            count=1+(span-1)//step
            for index in {0,1,count,count+1}:mapping(start,start+span,step,index)
for _ in range(2000):
    start=rng.randrange(1,n);end=rng.randrange(start+1,n+1)
    step=rng.randrange(2,min(n,1<<rng.randrange(2,257)))
    count=1+(end-start-1)//step
    mapping(start,end,step,rng.choice((1,count,rng.randrange(1,count+1),count+1)))
for start,end,step in [(0,3,2),(2,2,2),(3,2,2),(1,n+1,2),(1,n,0),(1,n,1),(1,n,n),(1,n,1<<256)]:mapping(start,end,step,1)
for bit in range(256):
    for step in (1,2,n-1,rng.randrange(1,n)):
        cases.append((f'power {step:x} {bit}',f'0x{(step*(1<<bit))%n:064x}'))
for command in ('power 0 1',f'power {n:x} 1','power 1 256','map 1 2 2 1 extra','map -1 2 2 1'):cases.append((command,'invalid'))
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=90)
assert r.returncode==0 and not r.stderr,(r.stdout,r.stderr)
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),seed=0xC2357D,oracle='Python arbitrary-precision integers',binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
