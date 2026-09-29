#!/usr/bin/env python3
"""Compare CPU point operations to the pinned library and independent affine math."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
from model import P, N, G, add, multiply, encode
from oracle_selftest import run, check_source

SEED=0xC060017


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--oracle',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    options=parser.parse_args()
    check_source()
    rng=random.Random(SEED)
    scalars=[0,1,2,3,4,N-1,N,N+1,(1<<256)-1]
    scalars += [x+d for x in (1<<32,1<<64,1<<128,1<<192,1<<255) for d in (-1,0,1)]
    scalars += [rng.randrange(1,N) for _ in range(96)]
    native=run(options.oracle,[f'pub {k:064x}' for k in scalars])
    points=[None,G,(G[0],P-G[1])]+[multiply(k) for k in scalars[9:25]]
    cases=[]

    def case(op,args,expected,copies=1):
        cases.append((op,op+' '+' '.join(args),' '.join([expected]*copies)))

    for k,expected in zip(scalars,native):
        case('pub',[f'{k:064x}'],expected)
        case('rawpub',[f'{k:064x}'],'inf' if expected=='invalid' else expected)
    for a in points:
        for z in (1,7,P-1):
            case('preduce',[encode(a),f'{z:064x}'],encode(a))
            case('pneg',[encode(a),f'{z:064x}'],encode(None if a is None else (a[0],-a[1]%P)),2)
            for op in ('pdouble','pdouble_direct'):
                case(op,[encode(a),f'{z:064x}'],encode(add(a,a)),2)
        for scalar in (0,1,2,3,7,N-1,N,N+1,rng.randrange(N)):
            case('pmul',[encode(a),f'{scalar:064x}'],encode(multiply(scalar,a)),2)
    pairs=[(a,b) for a in points[:4] for b in points[:4]]
    pairs += [(a,a) for a in points]+[(a,None) for a in points]
    pairs += [(a,None if a is None else (a[0],-a[1]%P)) for a in points]
    pairs += [tuple(rng.sample(points,2)) for _ in range(128)]
    sums=run(options.oracle,['add '+encode(a)+' '+encode(b) for a,b in pairs])
    for (a,b),expected in zip(pairs,sums):
        if expected!=encode(add(a,b)):
            raise RuntimeError('independent point oracles disagree')
        for za,zb in ((1,1),(7,1),(7,P-1)):
            for op in ('padd','padd2','padd_direct'):
                case(op,[encode(a),encode(b),f'{za:064x}',f'{zb:064x}'],expected,3)
    actual=run(options.binary,[c[1] for c in cases])
    failures=[{'operation':op,'command':c,'expected':e,'actual':a}
              for (op,c,e),a in zip(cases,actual) if e!=a]
    report={'seed':SEED,'cases':len(cases),'categories':dict(Counter(c[0] for c in cases)),
            'binary_sha256':hashlib.sha256(options.binary.read_bytes()).hexdigest(),
            'failure_count':len(failures),'failure_categories':dict(Counter(f['operation'] for f in failures)),
            'failures':failures[:20]}
    options.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return bool(failures)


if __name__=='__main__':
    raise SystemExit(main())
