#!/usr/bin/env python3
"""Check a mixed arithmetic tail batch on every visible HIP logical device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from model import P,N,G,encode
from oracle_selftest import run,check_source


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--oracle',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    pin=check_source()
    inventory=json.loads(subprocess.check_output([str(args.binary.resolve()),'devices','--backend','hip'],text=True,timeout=30))
    if not inventory['devices']: raise RuntimeError('HIP arithmetic requires visible hardware')
    scalars=[1,2,3,1<<64,1<<128,1<<192,N-1]
    public=dict(zip(scalars,run(args.oracle,[f'pub {k:064x}' for k in scalars])))
    cases=[]
    for i in range(129):
        which=i%6
        if which in (0,1):
            a=P-1-i; b=(1<<255)+i*(1<<32)
            cases.append((f'{"fmul" if which==0 else "fmul16"} {a:064x} {b:064x}',' '.join([f'{a*b%P:064x}']*3)))
        elif which==2:
            k=scalars[(i//6)%len(scalars)]; cases.append((f'pub {k:064x}',public[k]))
        elif which==3:
            cases.append((f'padd {encode(G)} {encode((G[0],P-G[1]))} {7:064x} {P-1:064x}','inf inf inf'))
        elif which==4:
            cases.append((f'pdouble {encode(G)} {i+1:064x}',public[2]+' '+public[2]))
        else:
            cases.append((f'finv {0:064x}',f'0 {0:064x} {0:064x}'))
    data='\n'.join(c for c,e in cases)+'\n'
    report={'oracle_commit':pin,'inventory':inventory,'cases_per_device':len(cases),
            'corpus_sha256':hashlib.sha256(data.encode()).hexdigest(),
            'binary_sha256':hashlib.sha256(args.probe.read_bytes()).hexdigest(),'devices':[],'failures':[]}
    with tempfile.TemporaryDirectory(prefix='keyhunt-c08-') as directory:
        for device in inventory['devices']:
            stats=Path(directory)/f"{device['ordinal']}.json"
            r=subprocess.run([str(args.probe.resolve()),'--device',str(device['ordinal']),'--batch','129','--stats',str(stats)],input=data,capture_output=True,text=True,timeout=60)
            if r.returncode or r.stderr or r.stdout.splitlines()!=[e for c,e in cases]:
                report['failures'].append({'device':device['ordinal'],'stderr':r.stderr,'exit_code':r.returncode,'output':r.stdout[:2000]})
            else:
                report['devices'].append({'ordinal':device['ordinal'],'uuid':device['uuid'],'stats':json.loads(stats.read_text())})
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"Arithmetic on {len(report['devices'])} devices, {len(cases)} cases each, {len(report['failures'])} failures")
    return bool(report['failures'])

if __name__=='__main__': raise SystemExit(main())
