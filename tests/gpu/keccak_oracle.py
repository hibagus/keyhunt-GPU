#!/usr/bin/env python3
"""Check every one-block Keccak padding boundary on portable and GPU paths."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from oracle.ethereum import digest
parser=argparse.ArgumentParser()
parser.add_argument('--binary',required=True)
parser.add_argument('--report',required=True)
args=parser.parse_args()
rng=random.Random(0xC23E)
cases=[]
for length in range(136):
    for data in (bytes(length),b'\xff'*length,rng.randbytes(length)):
        expected=digest(data).hex()
        assert expected != hashlib.sha3_256(data).hexdigest()
        cases.append((data,expected))
for length in (136,137,144):
    cases.append((bytes(length),'invalid'))
cases.append((b'abc',digest(b'abc').hex()))
process=subprocess.run([args.binary],input=''.join(f'0 {data.hex()}\n' for data,_ in cases),
                       text=True,capture_output=True,check=True,timeout=90)
actual=process.stdout.splitlines()
assert len(actual)==len(cases),(len(actual),len(cases))
for index,((data,expected),result) in enumerate(zip(cases,actual)):
    assert expected==result,(index,data.hex(),expected,result)
Path(args.report).write_text(json.dumps({'cases':len(cases),'oracle':'PyCryptodome 3.23.0 Keccak-256',
                                       'sha3_discrimination':True,'passed':True},indent=2)+'\n')
print(f'{len(cases)} independent Keccak cases passed')
