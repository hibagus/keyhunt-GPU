"""Pinned independent Keccak oracle, separate from retained CPU and GPU code."""
import os
import sys

# An isolated wheel extraction works on validation hosts without pip or root.
# Never silently substitute hashlib.sha3_256: that is a different algorithm.
if os.environ.get('KEYHUNT_KECCAK_ORACLE_ROOT'):
    sys.path.insert(0, os.environ['KEYHUNT_KECCAK_ORACLE_ROOT'])
import Crypto
from Crypto.Hash import keccak
if Crypto.__version__ != '3.23.0':
    raise RuntimeError('Ethereum tests require pinned PyCryptodome 3.23.0')

def digest(data):
    return keccak.new(digest_bits=256, data=data).digest()

assert digest(b'').hex() == 'c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470'
assert digest(b'abc').hex() == '4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45'

def address(public_hex):
    public = bytes.fromhex(public_hex)
    assert len(public) == 65 and public[0] == 4
    return digest(public[1:])[12:].hex()

def checksum(value):
    value = value.lower().removeprefix('0x')
    hashed = digest(value.encode('ascii')).hex()
    return '0x' + ''.join(c.upper() if int(hashed[i], 16) >= 8 else c for i,c in enumerate(value))
