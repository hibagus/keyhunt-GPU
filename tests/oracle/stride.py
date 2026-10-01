"""Independent targets/relations for affine scalar progressions; no production imports."""
from hash160 import hash160,address
from ethereum import address as ethereum_address

def targets(mode,public,overlap=False):
    if mode=='xpoint':
        lines={p[2:66] for p in public};canonical=sorted(lines)
    elif mode in ('hash160','address'):
        lines={hash160(p,tag) for p in public for tag in (1,2)}
        canonical=sorted(f'{tag:02x}'+v for v in lines for tag in (1,2))
        if mode=='address':lines={address(v) for v in lines}
    elif mode=='ethereum':
        lines={ethereum_address(p) for p in public};canonical=sorted(lines)
    else:
        lines={address(hash160(p,tag)) for p in public for tag in (1,2)}
        if overlap:lines|={'1'}
        canonical=sorted((bytes([tag,len(v)])+v.encode()+bytes(34-len(v))).hex() for v in lines for tag in (1,2))
    return sorted(lines),canonical

def relations(mode,public,canonical):
    if mode=='xpoint':wanted=[public[2:66]]
    elif mode in ('hash160','address'):wanted=[f'{tag:02x}'+hash160(public,tag) for tag in (1,2)]
    elif mode=='ethereum':wanted=[ethereum_address(public)]
    else:
        wanted=[]
        for raw in canonical:
            value=bytes.fromhex(raw);tag,length=value[:2]
            if address(hash160(public,tag)).startswith(value[2:2+length].decode()):wanted.append(raw)
    return [canonical.index(v) for v in wanted if v in canonical]
