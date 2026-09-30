#!/usr/bin/env python3
"""Independent format/hash/filter/baby-point checks and corrupted-cache rejection."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run

MASK=(1<<64)-1

def mix(x):
    x=((x^(x>>30))*0xbf58476d1ce4e5b9)&MASK
    x=((x^(x>>27))*0x94d049bb133111eb)&MASK
    return x^(x>>31)

def hash_key(key):
    h=0xcbf29ce484222325
    for byte in key: h=((h^byte)*0x100000001b3)&MASK
    return mix(h)

def parse(data):
    assert data[:8]==b'KHBSGS1\x00'
    assert hashlib.sha256(data[:-32]).digest()==data[-32:]
    m,buckets,words=struct.unpack_from('<QQQ',data,40)
    bits=struct.unpack_from('<I',data,64)[0]
    assert len(data)==128+8*(buckets+1)+48*m+8*words+32
    offsets=list(struct.unpack_from(f'<{buckets+1}Q',data,128))
    start=128+8*(buckets+1)
    entries=[(data[start+48*i:start+48*i+33],struct.unpack_from('<Q',data,start+48*i+40)[0]) for i in range(m)]
    bloom=list(struct.unpack_from(f'<{words}Q',data,start+48*m))
    assert sorted(j for key,j in entries)==list(range(m))
    assert offsets[0]==0 and offsets[-1]==m
    expected_bloom=[0]*words
    for b in range(buckets):
        assert entries[offsets[b]:offsets[b+1]]==sorted(entries[offsets[b]:offsets[b+1]])
        for key,j in entries[offsets[b]:offsets[b+1]]:
            h=hash_key(key); assert h&(buckets-1)==b
            for i in range(7):
                bit=((h+i*(mix(h^0x9e3779b97f4a7c15)|1))&MASK)&(words*64-1)
                expected_bloom[bit>>6] |= 1<<(bit&63)
    assert bloom==expected_bloom
    return m,bits,entries

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--oracle',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    report={'oracle_commit':check_source(),'tables':[],'rejections':[]}
    def run(*words): return subprocess.run([str(args.binary.resolve()),'bsgs-table',*map(str,words)],capture_output=True,text=True,timeout=60)
    public=oracle_run(args.oracle,[f'pub {j:064x}' for j in range(1,4097)])
    expected={0:b'\x00'*33,**{j:bytes.fromhex(('03' if int(pub[-2:],16)&1 else '02')+pub[2:66]) for j,pub in enumerate(public,1)}}
    with tempfile.TemporaryDirectory(prefix='keyhunt-c10-') as directory:
        root=Path(directory)
        for m,bits in [(1,8),(2,16),(7,32),(8,8),(9,16),(127,8),(128,16),(129,32),(257,16),(4097,16)]:
            path=root/f'{m}-{bits}.khb'
            r=run('build','--m',m,'--bits-per-entry',bits,'--output',path)
            assert r.returncode==0 and not r.stderr,r
            info=json.loads(r.stdout); data=path.read_bytes(); pm,pbits,entries=parse(data)
            assert (pm,pbits)==(m,bits) and all(key==expected[j] for key,j in entries)
            assert info['checksum']==data[-32:].hex() and not info['search_coverage']
            inspected=run('inspect','--input',path); assert inspected.returncode==0,inspected
            other=json.loads(inspected.stdout)
            assert {k:v for k,v in info.items() if k!='wall_ms'}=={k:v for k,v in other.items() if k!='wall_ms'}
            # Publication cannot replace an existing table, even with different m.
            exists=run('build','--m',1,'--output',path)
            assert exists.returncode==2 and path.read_bytes()==data and not list(root.glob('*.tmp.*'))
            report['tables'].append({'m':m,'bits':bits,'checksum':info['checksum'],'entries_checked':len(entries),'metadata':info})
        original=(root/'129-32.khb').read_bytes()
        mutated=root/'corrupt.khb'
        def reject(name,data,rehash=False,extra=()):
            data=bytearray(data)
            if rehash: data[-32:]=hashlib.sha256(data[:-32]).digest()
            mutated.write_bytes(data)
            r=run('inspect','--input',mutated,*extra)
            assert r.returncode==2 and not r.stdout,(name,r)
            report['rejections'].append({'name':name,'error':r.stderr.strip()})
        reject('legacy',b'legacy .blm table cache')
        reject('truncated_header',original[:100])
        reject('truncated_payload',original[:-1])
        reject('trailing_data',original+b'\x00')
        reject('checksum',original[:-1]+bytes([original[-1]^1]))
        reject('host_budget',original,extra=('--host-memory','1'))
        for offset in [8,12,16,20,24,28,32,36,40,48,56,64,68,72,80,88,96,104]:
            data=bytearray(original); data[offset]^=0x40
            reject(f'header_{offset}',data,True)
        m,buckets,words=struct.unpack_from('<QQQ',original,40)
        entries=128+8*(buckets+1); bloom=entries+48*m
        for name,offset in [('bucket_endpoint',128),('point',entries+2),('reserved_entry',entries+33),('index',entries+40)]:
            data=bytearray(original); data[offset]^=1; reject(name,data,True)
        data=bytearray(original)
        first,second=struct.unpack_from('<Q',data,entries+40)[0],struct.unpack_from('<Q',data,entries+88)[0]
        struct.pack_into('<Q',data,entries+40,second); struct.pack_into('<Q',data,entries+88,first)
        reject('permuted_jG_mapping',data,True)
        data=bytearray(original); data[bloom:bloom+8*words]=bytes(8*words)
        reject('filter_false_negative',data,True)
        data=bytearray(original); struct.pack_into('<Q',data,40,(1<<64)-1)
        reject('overflowing_m',data,True)
        # A saturated filter can only introduce false positives, never false negatives.
        data=bytearray(original); data[bloom:bloom+8*words]=b'\xff'*(8*words); data[-32:]=hashlib.sha256(data[:-32]).digest()
        mutated.write_bytes(data); assert run('inspect','--input',mutated).returncode==0
        for words in [[],['bad'],['build'],['build','--m','0','--output',root/'bad'],
                      ['build','--m','-1','--output',root/'bad'],['build','--m','18446744073709551616','--output',root/'bad'],
                      ['build','--m','1','--bits-per-entry','9','--output',root/'bad'],
                      ['inspect','--input',mutated,'--input',mutated],['inspect','--input',mutated,'--unknown','1']]:
            r=run(*words); assert r.returncode==2 and not r.stdout,r
            report['rejections'].append({'name':'arguments','error':r.stderr.strip()})
    report['binary_sha256']=hashlib.sha256(args.binary.read_bytes()).hexdigest()
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"BSGS: {len(report['tables'])} oracle tables, {sum(x['entries_checked'] for x in report['tables'])} baby entries, {len(report['rejections'])} rejections")
if __name__=='__main__': main()
