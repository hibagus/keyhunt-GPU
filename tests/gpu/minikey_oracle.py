#!/usr/bin/env python3
"""Probe every Base58 carry, integer-limb carry and public minikey hash vector."""
import argparse,json,random,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from minikey import text,ordinal,scalar,PUBLIC_KEYS
p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--report',type=Path,required=True);a=p.parse_args()
cases=[];rng=random.Random(0xC236)
for length,key in zip((22,30),PUBLIC_KEYS):
    end=58**(length-1)+1
    values={0,1,2,end-2,end-1,end,end+1,ordinal(key)}
    for power in range(1,length):values|={58**power+d for d in (-1,0,1,2)}
    for bit in (32,64,96,128,160):
        if 2**bit<end:values|={2**bit+d for d in (-1,0,1)}
    values|={rng.randrange(1,end) for _ in range(768)}
    for value in sorted(values):
        expected='invalid'
        if 1<=value<end:
            candidate=text(value,length);derived=scalar(candidate);expected=candidate+' '+(f'{derived:064x}' if derived else 'rejected')
        cases.append((bytes([length])+value.to_bytes(32,'big'),expected))
for length in (0,21,23,26,29,31,255):cases.append((bytes([length])+bytes(31)+b'\1','invalid'))
for width in (0,1,31,32,34,144):cases.append((bytes(width),'invalid'))
result=subprocess.run([str(a.binary.resolve())],input=''.join('0 '+data.hex()+'\n' for data,_ in cases),text=True,capture_output=True,timeout=90)
assert result.returncode==0,(result.stdout,result.stderr)
assert result.stdout.splitlines()==[expected for _,expected in cases]
report=dict(passed=True,cases=len(cases),oracle='Python bigint Base58 ordinals and hashlib SHA-256',admitted=sum(' ' in v and not v.endswith('rejected') for _,v in cases))
a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
