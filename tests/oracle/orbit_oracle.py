#!/usr/bin/env python3
"""Independent big-integer six-member mapping and canonical inverse oracle."""
import argparse,hashlib,json,random,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from model import N
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();rng=random.Random(0xC2306);L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
factors=[pow(L,v//2,N)*(-1 if v%2 else 1)%N for v in range(6)]
assert len(set(factors))==6 and pow(L,3,N)==1
cases=[]
def add(begin,end,stride,reverse,index):
 command=f'{begin:x} {end:x} {stride:x} {reverse} {index:x}';expected='invalid'
 if 1<=begin<end<=N and 1<=stride<N:
  count=1+(end-begin-1)//stride
  if 6*count+1<=N and 1<=index<=6*count:
   v,i=divmod(index-1,count);seed=begin+((count-i-1) if reverse else i)*stride
   expected=f'0x{6*count+1:064x} 0x{seed:064x} {v} 0x{seed*factors[v]%N:064x} 0x{index:064x} 0x{(v+1)*count+1:064x}'
 cases.append((command,expected))
for begin in (1,7,N-33):
 for span in (1,2,3,17,33):
  for stride in (1,2,7,19):
   for reverse in (0,1):
    for index in range(6*(1+(span-1)//stride)+2):add(begin,begin+span,stride,reverse,index)
for _ in range(512):
 begin=rng.randrange(1,N-1);end=rng.randrange(begin+1,N+1);stride=rng.randrange(1,N)
 count=1+(end-begin-1)//stride
 for v in range(6):add(begin,end,stride,rng.randrange(2),v*count+rng.randrange(1,count+1))
limit=(N-1)//6
for count in (limit-1,limit,limit+1,N-1):
 for reverse in (0,1):
  for index in (1,count,6*count):add(1,count+1,1,reverse,index)
for args in ((0,2,1,0,1),(1,2,0,0,1),(1,2,N,0,1),(1,N+1,1,0,1)):add(*args)
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=120)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
