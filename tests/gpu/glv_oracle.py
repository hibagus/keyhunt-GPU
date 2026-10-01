#!/usr/bin/env python3
"""Independent integer decomposition and pinned full-public-key GLV oracle."""
import argparse,hashlib,json,random,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import N,P,G,multiply
from oracle_selftest import check_source,run as oracle_run
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();rng=random.Random(0xC2361)
A1=int('3086d221a7d46bcde86c90e49284eb15',16)
B1=-int('e4437ed6010e88286f547fa90abfe4c3',16)
A2=int('114ca50f7a8e2f3f657c1108d9d44cfd8',16);B2=A1
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
BETA=int('7ae96a2b657c07106e64479eac3434e99cf0497512f58995c1396c28719501ee',16)
assert A1*B2-B1*A2==N and (A1+L*B1)%N==(A2+L*B2)%N==0
assert pow(L,3,N)==pow(BETA,3,P)==1 and L!=1 and BETA!=1
assert multiply(L)==(G[0]*BETA%P,G[1])
# Derive the reciprocal constants using arbitrary-precision division. The
# production code uses fixed 32-bit limbs and does not call these helpers.
def nearest(n,d):return (n+d//2)//d
g1=nearest((1<<384)*B2,N);g2=nearest((1<<384)*(-B1),N)
def split(k):
 c1=nearest(k*g1,1<<384);c2=nearest(k*g2,1<<384)
 first=k-c1*A1-c2*A2;second=-(c1*B1+c2*B2)
 assert abs(first)<1<<128 and abs(second)<1<<128 and (first+L*second)%N==k
 return first,second
values={0,1,2,N-1,N,(1<<256)-1}
for bit in range(256):
 for delta in (-1,0,1):values.add(max(0,min((1<<256)-1,(1<<bit)+delta)))
for coefficient in (B2,-B1):
 for c in [0,1,2,*[1<<bit for bit in range(128)],*[rng.randrange(1,coefficient) for _ in range(128)]]:
  boundary=((2*c+1)*N)//(2*coefficient)
  for delta in (-2,-1,0,1,2):
   if 0<boundary+delta<N:values.add(boundary+delta)
values.update(rng.randrange(1,N) for _ in range(2000))
cases=[];signs=set()
for k in sorted(values):
 if 0<k<N:
  first,second=split(k);signs.add((first<0,second<0))
  expected=('+' if first>=0 else '-')+f'{abs(first):032x} '+('+' if second>=0 else '-')+f'{abs(second):032x}'
 else:expected='invalid'
 cases.append((f'0 {k:064x}',expected))
assert len(signs)==4
points=sorted({1,2,N-1,*range(3,66),*[N-i for i in range(2,34)],*[1<<bit for bit in range(256)],*[rng.randrange(1,N) for _ in range(256)]})
public=oracle_run(a.oracle,[f'pub {k:064x}' for k in points])
cases.extend((f'1 {k:064x}',pub) for k,pub in zip(points,public))
phi=points[::17];phi_public=oracle_run(a.oracle,[f'pub {k*L%N:064x}' for k in phi])
cases.extend((f'2 {k:064x}',pub) for k,pub in zip(phi,phi_public))
for operation in (1,2):
 for k in (0,N,(1<<256)-1):cases.append((f'{operation} {k:064x}','invalid'))
assert len(cases)<=8192
r=subprocess.run([str(a.binary.resolve())],input='\n'.join(c for c,_ in cases)+'\n',capture_output=True,text=True,timeout=150)
assert r.returncode==0 and not r.stderr,(r.stdout[-1000:],r.stderr)
actual=r.stdout.splitlines();assert len(actual)==len(cases)
failures=[dict(command=c,expected=e,actual=v) for (c,e),v in zip(cases,actual) if e!=v]
report=dict(passed=not failures,cases=len(cases),decompositions=len(values),public_keys=len(points),endomorphism_points=len(phi),sign_combinations=len(signs),
 oracle='Python arbitrary-precision integers and pinned libsecp256k1',oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),failures=failures[:10])
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report));raise SystemExit(bool(failures))
