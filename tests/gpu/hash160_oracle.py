#!/usr/bin/env python3
"""Independent OpenSSL/hashlib oracle for portable and real-device short hashes."""
import argparse
import hashlib
import json
import random
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--report', required=True)
args = parser.parse_args()
rng = random.Random(0xC230)
cases = []
# OpenSSL 3 may keep RIPEMD-160 in its explicitly loaded legacy provider.
# Only this independent test process opts into that provider.
try:
    hashlib.new('ripemd160', b'')
    def ripemd(data):
        return hashlib.new('ripemd160', data).hexdigest()
except ValueError:
    def ripemd(data):
        return subprocess.run(['openssl', 'dgst', '-provider', 'default', '-provider', 'legacy',
                               '-ripemd160', '-binary'], input=data, capture_output=True,
                              check=True, timeout=10).stdout.hex()
assert ripemd(b'') == '9c1185a5c5e9fc54612808977ee8f548b2258d31'
assert ripemd(b'abc') == '8eb208f7e05d987a9b044a8e98c6b087f15a0bfc' 
# All supported lengths exercise padding boundaries and both SHA blocks. Random
# bytes catch byte-order errors that all-zero vectors alone would miss.
for operation, maximum in ((0, 119), (1, 55)):
    for size in range(maximum + 1):
        for data in (bytes(size), bytes([255]) * size, rng.randbytes(size)):
            expected = hashlib.sha256(data).hexdigest() if operation == 0 else ripemd(data)
            cases.append((operation, data, expected))
    for size in (maximum + 1, 128):
        cases.append((operation, bytes(size), 'invalid'))
for size in (33, 65):
    for _ in range(64):
        data = rng.randbytes(size)
        cases.append((2, data, ripemd(hashlib.sha256(data).digest())))
for size in (0, 32, 34, 64, 66, 128):
    cases.append((2, bytes(size), 'invalid'))
# Published public-scalar-1 vector checks the complete composition separately.
public = bytes.fromhex('0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798')
cases.append((2, public, '751e76e8199196d454941c45d1b3a323f1433bd6'))
process = subprocess.run([args.binary], input=''.join(f'{op} {data.hex()}\n' for op, data, _ in cases),
                         text=True, capture_output=True, timeout=90, check=True)
actual = process.stdout.splitlines()
assert len(actual) == len(cases), (len(actual), len(cases), process.stderr)
for index, ((op, data, expected), result) in enumerate(zip(cases, actual)):
    assert result == expected, (index, op, data.hex(), expected, result)
with open(args.report, 'w') as report:
    json.dump({'cases': len(cases), 'oracle': 'Python hashlib/OpenSSL', 'passed': True}, report, indent=2)
    report.write('\n')
print(f'{len(cases)} independent SHA-256/RIPEMD-160/HASH160 cases passed')
