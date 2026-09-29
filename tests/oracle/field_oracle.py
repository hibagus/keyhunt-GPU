#!/usr/bin/env python3
"""CPU field/scalar arithmetic differential tests against exact Python integers."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
from model import P, N
from oracle_selftest import run

SEED = 0xC06F1E1D


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    options=parser.parse_args()
    rng=random.Random(SEED)
    cases=[]

    def case(op, values, result, copies=1):
        cases.append((op, op+' '+ ' '.join(f'{v:064x}' for v in values),
                      ' '.join([f'{result:064x}']*copies)))

    edges={0,1,2,P-2,P-1}
    for bit in (31,32,33,63,64,65,127,128,129,191,192,193,224,255):
        edges.update(((1<<bit)-1,1<<bit,(1<<bit)+1))
    pairs=[(a,b) for a in sorted(edges) for b in sorted(edges)]
    pairs += [(rng.randrange(P),rng.randrange(P)) for _ in range(1000)]
    for a,b in pairs:
        case('fadd',(a,b),(a+b)%P,3)
        case('fsub',(a,b),(a-b)%P,3)
        case('fmul',(a,b),(a*b)%P,4)
    for a in sorted(edges)+[rng.randrange(P) for _ in range(512)]:
        case('fsquare',(a,),a*a%P,2)
        case('fmulself',(a,),a*a%P)
        case('finv',(a,),pow(a,-1,P) if a else 0)
        case('fneg',(a,),-a%P)
    # Specialized multiplication/squaring promises a reduced result even for
    # full 256-bit operands at and immediately above the field prime.
    for a in (P,P+1,(1<<256)-1):
        case('fsquare',(a,),a*a%P,2)
        case('fmulself',(a,),a*a%P)
        for b in (0,1,2,P-1,P,P+1,(1<<256)-1):
            case('fmul',(a,b),a*b%P,4)
    scalar_edges=[0,1,2,N-2,N-1]+[1<<bit for bit in (32,64,128,192,255)]
    scalar_pairs=[(a,b) for a in scalar_edges for b in scalar_edges]
    scalar_pairs += [(rng.randrange(N),rng.randrange(N)) for _ in range(512)]
    for a in scalar_edges+[rng.randrange(N) for _ in range(128)]:
        case('smulself',(a,),a*a%N)
    for a,b in scalar_pairs:
        case('sadd',(a,b),(a+b)%N,3)
        case('smul',(a,b),a*b%N,2)
    actual=run(options.binary,[c[1] for c in cases])
    failures=[{'operation':op,'command':c,'expected':e,'actual':a}
              for (op,c,e),a in zip(cases,actual) if a!=e]
    report={'seed':SEED,'cases':len(cases),'categories':dict(Counter(c[0] for c in cases)),
            'binary_sha256':hashlib.sha256(options.binary.read_bytes()).hexdigest(),
            'failure_count':len(failures),'failures':failures[:20]}
    options.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return bool(failures)


if __name__=='__main__':
    raise SystemExit(main())
