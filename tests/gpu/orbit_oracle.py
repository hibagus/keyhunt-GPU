#!/usr/bin/env python3
"""Six point transforms checked against independently multiplied private scalars."""
import argparse,hashlib,json,random,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N
from oracle_selftest import check_source,run as oracle_run
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();rng=random.Random(0xC23066)
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
seeds=sorted({1,2,N-1,*[N-i for i in range(2,34)],*[1<<bit for bit in range(256)],*[rng.randrange(1,N) for _ in range(128)]})
requests=[];scalars=[]
for seed in seeds:
 for v in range(6):
  requests.append(f'{v+3} {seed:064x}')
  scalars.append(seed*pow(L,v//2,N)*(-1 if v%2 else 1)%N)
expected=oracle_run(a.oracle,[f'pub {k:064x}' for k in scalars])
for v in range(6):
 for bad in (0,N,(1<<256)-1):requests.append(f'{v+3} {bad:064x}');expected.append('invalid')
requests.append('9 '+f'{1:064x}');expected.append('invalid')
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(requests)+'\n',capture_output=True,text=True,timeout=150)
assert r.returncode==0 and not r.stderr,r.stderr
actual=r.stdout.splitlines();assert len(actual)==len(expected)
failures=[dict(request=c,expected=e,actual=v) for c,e,v in zip(requests,expected,actual) if e!=v]
report=dict(passed=not failures,cases=len(requests),seeds=len(seeds),oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
