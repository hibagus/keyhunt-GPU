"""Independent ordinal/string mapping and SHA-256 admission for public fixtures."""
import hashlib
ALPHABET='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
N=0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
PUBLIC_KEYS=('SzavMBLoXU6kDrqtUVmffv','S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy')
def text(ordinal,length):
    if length not in (22,30) or not 1<=ordinal<=58**(length-1):raise ValueError('invalid ordinal/length')
    number=ordinal-1;digits=''
    for _ in range(length-1):number,digit=divmod(number,58);digits=ALPHABET[digit]+digits
    return 'S'+digits
def ordinal(candidate):
    assert len(candidate) in (22,30) and candidate[0]=='S'
    number=0
    for c in candidate[1:]:number=number*58+ALPHABET.index(c)
    return number+1
def scalar(candidate):
    if hashlib.sha256((candidate+'?').encode()).digest()[0]:return None
    value=int.from_bytes(hashlib.sha256(candidate.encode()).digest(),'big')
    return value if 0<value<N else None
assert scalar(PUBLIC_KEYS[0])==0xe9873d79c6d87dc0fb6a5778633389f4453213303da61f20bd67fc233aa33262
assert scalar(PUBLIC_KEYS[1])==0x4c7a9640c72dc2099f23715d0c8a0d8a35f8906e3cab61dd3f78b67bf887c9ab
