#!/usr/bin/env python3
"""Portable/HIP field results versus exact Python integers and legacy CPU math."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'oracle'))
from model import P
from oracle_selftest import run

SEED = 0xC08F1E1D

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--cpu', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--device', default='0')
    args = parser.parse_args()
    rng = random.Random(SEED)
    cases, cpu_cases = [], []
    def case(op, values, output, cpu_output=None):
        command = op + ' ' + ' '.join(f'{v:064x}' for v in values)
        cases.append((op, command.rstrip(), output))
        if cpu_output is not None: cpu_cases.append((command, cpu_output))
    edges = {0,1,2,P-2,P-1,P,P+1,(1<<256)-1}
    for bit in (31,32,33,63,64,65,127,128,191,192,224,255):
        edges.update(((1<<bit)-1, 1<<bit, (1<<bit)+1))
    pairs = [(a,b) for a in sorted(edges) for b in sorted(edges)]
    pairs += [(rng.getrandbits(256),rng.getrandbits(256)) for _ in range(1024)]
    for a,b in pairs:
        for op, result in [('fadd',(a+b)%P),('fsub',(a-b)%P),('fmul',a*b%P),('fmul16',a*b%P)]:
            value=f'{result:064x}'
            case(op,(a,b),' '.join([value]*3), value if a<P and b<P and op != 'fmul16' else None)
    for a in sorted(edges)+[rng.getrandbits(256) for _ in range(256)]:
        reduced=a%P
        case('fnorm',(a,),f'{reduced:064x}')
        case('fbytes',(a,),f'{int(a<P)} {a if a<P else 0:064x} {reduced:064x}')
        for op,value in [('fsquare',a*a%P),('fneg',-a%P)]:
            case(op,(a,),f'{value:064x} {value:064x}',f'{value:064x}' if a<P else None)
        inv=pow(reduced,-1,P) if reduced else 0
        case('finv',(a,),f'{int(bool(reduced))} {inv:064x} {inv:064x}',f'{inv:064x}' if a<P else None)
    for count in (0,1,2,3,7,16,31,32):
        for pattern in ('all_zero','first_zero','last_zero','alternating','nonzero'):
            values=[rng.randrange(1,P) for _ in range(count)]
            for i in range(count):
                if pattern=='all_zero' or (pattern=='first_zero' and i==0) or (pattern=='last_zero' and i==count-1) or (pattern=='alternating' and i%2): values[i]=0
            expected=' '.join(f'{pow(v,-1,P) if v else 0:064x}' for v in values)
            case('finvgroup',values,'1'+(' '+expected if expected else ''),None)
    case('flimits',(),f'{0:064x}')
    # Check raw carry/borrow output independently of field reduction, including
    # ripple chains across every limb and aliases of both input arrays.
    word_pairs = pairs.copy()
    for bit in range(32, 256, 32):
        word_pairs += [((1<<bit)-1, 1), (1, (1<<bit)-1), (1<<bit, 1), (1, 1<<bit)]
    mask = (1<<256)-1
    for a, b in word_pairs:
        for op, value, flag in [('wadd', a+b, (a+b)>>256), ('wsub', a-b, int(a<b))]:
            expected = ' '.join([f'{value & mask:064x}']*3)
            case(op, (a,b), f'{flag} {expected}')
    stats_path=args.report.with_suffix('.stats.json')
    command=[str(args.binary.resolve()),'--device',args.device,'--batch','257','--stats',str(stats_path)]
    process=subprocess.run(command,input='\n'.join(c[1] for c in cases)+'\n',capture_output=True,text=True,timeout=150)
    if process.returncode or process.stderr: raise RuntimeError(process.stderr or f'probe exit {process.returncode}')
    actual=process.stdout.splitlines()
    if len(actual)!=len(cases): raise RuntimeError('probe result count mismatch')
    failures=[{'operation':op,'command':c,'expected':e,'actual':a} for (op,c,e),a in zip(cases,actual) if e!=a]
    cpu_actual=run(args.cpu,[c for c,e in cpu_cases])
    cpu_failures=[{'command':c,'expected':e,'actual':a} for (c,e),a in zip(cpu_cases,cpu_actual) if a.split()[0]!=e]
    # Isolate one-item and workgroup-boundary launches, in addition to the full
    # corpus's repeated 257-item batches. Every device launch checks a tail guard.
    boundary_runs=[]
    for size in (1,127,128,129,256,257,1024):
        selected=[cases[(i*31)%len(cases)] for i in range(size)]
        r=subprocess.run([str(args.binary.resolve()),'--device',args.device,'--batch',str(size)],
                         input='\n'.join(c[1] for c in selected)+'\n',capture_output=True,text=True,timeout=30)
        if r.returncode or r.stderr or r.stdout.splitlines()!=[c[2] for c in selected]:
            failures.append({'batch_size':size,'error':r.stderr,'exit_code':r.returncode})
        boundary_runs.append(size)
    report={'seed':SEED,'cases':len(cases),'cpu_comparisons':len(cpu_cases),
            'categories':dict(Counter(c[0] for c in cases)),'boundary_batch_sizes':boundary_runs,
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'stats':json.loads(stats_path.read_text()),'failure_count':len(failures)+len(cpu_failures),
            'failures':failures[:20],'cpu_failures':cpu_failures[:20]}
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return bool(report['failure_count'])

if __name__=='__main__': raise SystemExit(main())
