"""Independent OpenSSL HASH160 and Base58Check fixtures; no production code."""
import ctypes
import ctypes.util
import functools
import hashlib

try:
    hashlib.new('ripemd160', b'')
    def ripemd(data):
        return hashlib.new('ripemd160', data).digest()
except ValueError:
    # OpenSSL 3's hashlib provider can omit RIPEMD-160. Its separately compiled
    # low-level implementation remains available without changing process policy.
    _crypto = ctypes.CDLL(ctypes.util.find_library('crypto'))
    _ripemd = _crypto.RIPEMD160
    _ripemd.argtypes = (ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)
    _ripemd.restype = ctypes.c_void_p
    def ripemd(data):
        output = ctypes.create_string_buffer(20)
        if not _ripemd(data, len(data), output):
            raise RuntimeError('OpenSSL RIPEMD160 failed')
        return output.raw

assert ripemd(b'').hex() == '9c1185a5c5e9fc54612808977ee8f548b2258d31'
assert ripemd(b'abc').hex() == '8eb208f7e05d987a9b044a8e98c6b087f15a0bfc'

@functools.lru_cache(maxsize=32768)
def hash160(public, tag):
    assert len(public) == 130 and public[:2] == '04' and tag in (1, 2)
    data = bytes.fromhex(public)
    if tag == 1:
        data = bytes([2 + (data[-1] & 1)]) + data[1:33]
    return ripemd(hashlib.sha256(data).digest()).hex()

def address(digest):
    payload = b'\0' + bytes.fromhex(digest)
    data = payload + hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    number = int.from_bytes(data, 'big')
    text = ''
    alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
    while number:
        number, digit = divmod(number, 58)
        text = alphabet[digit] + text
    return '1' * (len(data) - len(data.lstrip(b'\0'))) + text
