#!/usr/bin/env python3
"""GPU/shared point math versus libsecp256k1, affine Python, and legacy CPU math."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import P,N,G,add,multiply,encode
from oracle_selftest import run,check_source

SEED=0xC080017

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--cpu',type=Path,required=True)
    parser.add_argument('--oracle',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--device',default='0')
    args=parser.parse_args()
    pin=check_source()
    rng=random.Random(SEED)
    scalars=[v['scalar'] for v in json.loads((Path(__file__).resolve().parents[1]/'oracle/vectors.json').read_text())['vectors']]
    scalars += [f'{rng.randrange(1,N):064x}' for _ in range(64)]
    public=run(args.oracle,['pub '+k for k in scalars])
    cases=[]; cpu_cases=[]
    def case(op,words,expected,copies=1,cpu_op=None,cpu=True):
        command=op+' '+' '.join(words)
        cases.append((op,command,' '.join([expected]*copies)))
        if cpu: cpu_cases.append(((cpu_op or op)+' '+' '.join(words),expected))
    for k,expected in zip(scalars,public):
        model=encode(multiply(int(k,16))) if 0<int(k,16)<N else 'invalid'
        if model!=expected: raise RuntimeError('native and Python public-key oracles disagree')
        case('pub',[k],expected)
    points=[None,G,(G[0],P-G[1])]+[multiply(int(k,16)) for k in scalars[-12:]]
    for point in points:
        for z in (1,7,P-1):
            case('preduce',[encode(point),f'{z:064x}'],encode(point))
            case('pdouble',[encode(point),f'{z:064x}'],encode(add(point,point)),2)
            case('pneg',[encode(point),f'{z:064x}'],encode(None if point is None else (point[0],-point[1]%P)),2)
        for k in (0,1,2,3,N-1,N,N+1,(1<<256)-1,rng.randrange(N)):
            case('pmul',[encode(point),f'{k:064x}'],encode(multiply(k,point)),2)
    pairs=[(a,b) for a in points[:3] for b in points[:3]]
    pairs += [(a,a) for a in points]+[(a,None) for a in points]
    pairs += [(a,None if a is None else (a[0],-a[1]%P)) for a in points]
    pairs += [tuple(rng.sample(points,2)) for _ in range(96)]
    sums=run(args.oracle,['add '+encode(a)+' '+encode(b) for a,b in pairs])
    for (a,b),expected in zip(pairs,sums):
        if expected!=encode(add(a,b)): raise RuntimeError('native and Python addition oracles disagree')
        for za,zb in ((1,1),(7,1),(7,P-1)):
            words=[encode(a),encode(b),f'{za:064x}',f'{zb:064x}']
            case('padd',words,expected,3)
            case('pmixed',words,expected,2,cpu_op='padd')
    validation=[(G,True),((G[0],P-G[1]),True),((0,0),False),((G[0],0),False),
                ((G[0],(G[1]+1)%P),False),((P,G[1]),False),((G[0],P),False),(((1<<256)-1,G[1]),False)]
    for point,valid in validation:
        native=run(args.oracle,['parse '+encode(point)])[0]
        if (native!='invalid')!=valid: raise RuntimeError('invalid-point oracle disagreement')
        case('pvalid',[encode(point)],str(int(valid)),cpu=False)
    stats=args.report.with_suffix('.stats.json')
    command=[str(args.binary.resolve()),'--device',args.device,'--batch','257','--stats',str(stats)]
    result=subprocess.run(command,input='\n'.join(c[1] for c in cases)+'\n',capture_output=True,text=True,timeout=150)
    if result.returncode or result.stderr: raise RuntimeError(result.stderr or f'probe exit {result.returncode}')
    actual=result.stdout.splitlines()
    if len(actual)!=len(cases): raise RuntimeError('result count mismatch')
    failures=[{'operation':op,'command':c,'expected':e,'actual':a} for (op,c,e),a in zip(cases,actual) if e!=a]
    cpu=run(args.cpu,[c for c,e in cpu_cases])
    cpu_failures=[{'command':c,'expected':e,'actual':a} for (c,e),a in zip(cpu_cases,cpu) if a.split()[0]!=e]
    boundaries=[]
    # Use point-only workgroups and a final tail, not merely field-heavy kernels.
    for size in (1,127,128,129,257):
        selected=[cases[(i*37)%len(cases)] for i in range(size)]
        r=subprocess.run([str(args.binary.resolve()),'--device',args.device,'--batch',str(size)],input='\n'.join(c[1] for c in selected)+'\n',capture_output=True,text=True,timeout=60)
        if r.returncode or r.stderr or r.stdout.splitlines()!=[c[2] for c in selected]: failures.append({'batch_size':size,'error':r.stderr,'exit_code':r.returncode})
        boundaries.append(size)
    report={'seed':SEED,'oracle_commit':pin,'cases':len(cases),'cpu_comparisons':len(cpu_cases),
            'categories':dict(Counter(c[0] for c in cases)),'boundary_batch_sizes':boundaries,
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'stats':json.loads(stats.read_text()),
            'failure_count':len(failures)+len(cpu_failures),'failures':failures[:20],'cpu_failures':cpu_failures[:20]}
    args.report.write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report,indent=2))
    return bool(report['failure_count'])

if __name__=='__main__': raise SystemExit(main())
