#!/usr/bin/env python3
"""Independent Python bigint/Base58Check oracle for real-device address text."""
import argparse,json,random,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from hash160 import address
p=argparse.ArgumentParser();p.add_argument('--binary',required=True);p.add_argument('--report',required=True);a=p.parse_args()
rng=random.Random(0xC23B)
values=[bytes(20),b'\xff'*20]
# Every leading-zero length, byte position and carry edge participates.
for zeros in range(20):
    for fill in (1,127,128,255):values.append(bytes(zeros)+bytes([fill])*(20-zeros))
values += [rng.randbytes(20) for _ in range(256)]
values += [bytes.fromhex(h) for h in ('751e76e8199196d454941c45d1b3a323f1433bd6','91b24bf9f5288532960ac687abb035127b1d28a5')]
cases=[(v,address(v.hex())) for v in values]+[(bytes(n),'invalid') for n in (0,1,19,21,144)]
assert cases[0][1]=='1111111111111111111114oLvT2'
result=subprocess.run([a.binary],input=''.join('0 '+v.hex()+'\n' for v,_ in cases),text=True,capture_output=True,check=True,timeout=90)
actual=result.stdout.splitlines();assert len(actual)==len(cases)
for i,((value,expected),got) in enumerate(zip(cases,actual)):assert got==expected,(i,value.hex(),expected,got)
Path(a.report).write_text(json.dumps({'passed':True,'cases':len(cases),'oracle':'Python bigint Base58 and hashlib SHA-256'},indent=2)+'\n')
print(f'{len(cases)} independent Base58Check cases passed')
