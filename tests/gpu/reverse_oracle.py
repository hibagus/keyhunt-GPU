#!/usr/bin/env python3
"""Independent checked GPU scalar multiply-subtract, including rejected underflow."""
import argparse,hashlib,json,random,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
n=0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
rng=random.Random(0xC2357D);cases=[]
def add(begin,stride,offset):
    result=begin-stride*offset
    expected=f'{result:064x}' if offset<=0xffffffff and 1<=begin<n and 1<=stride<n and 1<=result<n else 'invalid'
    cases.append(('1 '+f'{begin:064x}{stride:064x}{offset:016x}',expected))
for bit in (0,1,31,32,63,64,95,96,127,128,159,160,191,192,223,224,254,255):
    for step in (1,(1<<bit),n-1):
        for offset in (0,1,2,7,255,1048575,0xffffffff,0x100000000):add((1<<bit)-1,step,offset)
for _ in range(2000):
    offset=rng.randrange(1,1048576)
    begin=rng.randrange(1,n)
    step=rng.randrange(1,max(2,(begin-1)//offset)) if rng.randrange(2) else rng.randrange(1,n)
    add(begin,step,offset)
for begin in (0,1,n-1,n,(1<<256)-1):
    for step in (0,1,n-1,n,(1<<256)-1):
        for offset in (0,1,0xffffffffffffffff):add(begin,step,offset)
cases += [('1 ','invalid'),('1 01','invalid')]
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=90)
assert r.returncode==0 and not r.stderr,(r.stdout,r.stderr)
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),oracle='Python arbitrary-precision integers',binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
