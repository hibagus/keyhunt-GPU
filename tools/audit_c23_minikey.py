#!/usr/bin/env python3
"""Independent public-fixture oracle and raw timing checks for the audit experiment."""
import argparse, functools, hashlib, json, math, pathlib, statistics, sys
p=argparse.ArgumentParser();p.add_argument('--source',type=pathlib.Path,required=True);p.add_argument('--input',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
sys.path.insert(0,str(a.source/'tests/oracle'))
from model import multiply,encode,N
from hash160 import hash160
ALPHABET=b'123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
INDEX={v:i for i,v in enumerate(ALPHABET)}
def require(condition,why):
    if not condition:raise ValueError(why)
@functools.lru_cache(None)
def enumerate_valid(initial,count):
    candidate=bytearray(initial.encode());valid=[]
    for offset in range(count):
        if hashlib.sha256(candidate+b'?').digest()[0]==0:
            scalar=int.from_bytes(hashlib.sha256(candidate).digest(),'big')
            if 0<scalar<N:valid.append((offset,scalar))
        if offset+1<count:
            for pos in range(len(candidate)-1,0,-1):
                digit=INDEX[candidate[pos]]
                candidate[pos]=ALPHABET[(digit+1)%58]
                if digit<57:break
            else:raise ValueError('fixture overrun')
    return tuple(valid)
@functools.lru_cache(None)
def targets(length,scalar):
    point=encode(multiply(scalar))
    return tuple(bytes([length,tag])+bytes.fromhex(hash160(point,tag)) for tag in (1,2))
def ordinal(text):
    v=0
    for c in text.encode()[1:]:v=v*58+INDEX[c]
    return v+1
report=json.loads(a.input.read_text());require(report['passed'],'GPU experiment did not pass')
require(len(report['cases']) in (44,48),'unexpected workload count')
variants=report.get('variants',['baseline','packed-direct','packed-glv']);require(variants[0]=='baseline' and len(set(variants))==len(variants),'invalid variants')
rows=[];checked=0;attempts=0
for work in report['cases']:
    valid=enumerate_valid(work['initial'],work['count'])
    require(len(valid)==work['cpu_admitted'],'independent admission count differs')
    require(ordinal(work['initial'])==int(work['begin'],16),'independent ordinal differs')
    require(work['scratch_bytes']==40*(work['count']+1),'scratch allocation differs')
    expected_targets=set()
    for i,(_,scalar) in enumerate(valid):
        if not work['measured'] or i in (0,len(valid)-1):expected_targets.update(targets(work['length'],scalar))
    if not valid:expected_targets.add(targets(work['length'],int.from_bytes(hashlib.sha256(b'SzavMBLoXU6kDrqtUVmffv').digest(),'big'))[0])
    target_list=sorted(expected_targets)
    require([list(t) for t in target_list]==work['targets'],'independent canonical targets differ')
    # Check every admitted scalar against all canonical targets, including the
    # measured cases; scalar multiplication here is Python affine mathematics.
    expected=set()
    for offset,scalar in valid:
        for target in targets(work['length'],scalar):
            if target in expected_targets:expected.add((work['count']-1-offset if work['reverse'] else offset,target_list.index(target)))
    require(sorted(map(list,expected))==work['expected_relations'],'independent offset/target relations differ')
    require(len(expected)==work['expected_matches'],'independent match count differs')
    measured=work['measured'];rounds=9 if measured else 1
    samples=work['samples'];require(len(samples)==len(variants)*rounds,'sample count')
    medians={};ratios={}
    for kind in variants:
        subset=[v for v in samples if v['variant']==kind]
        require(sorted(v['round'] for v in subset)==list(range(rounds)),'missing/duplicate round')
        for sample in subset:
            require(sample['steps']==work['count'] and sample['matches']==len(expected),'sample coverage')
            require(sample['overflow']==(len(expected)>work['capacity']),'sample overflow')
            for metric in ['kernel_ms','wall_ms']:require(math.isfinite(sample[metric]) and sample[metric]>0,'invalid timing')
        medians[kind]={metric:statistics.median(v[metric] for v in subset) for metric in ['kernel_ms','wall_ms','filter_ms']}
        if kind!='baseline':
            values={}
            for metric in ['kernel_ms','wall_ms']:
                pairs=[next(v[metric] for v in samples if v['variant']=='baseline' and v['round']==s['round'])/s[metric] for s in subset]
                values[metric]={'median':statistics.median(pairs),'min':min(pairs),'max':max(pairs),'pairs':pairs}
            ratios[kind]=values
    rows.append(dict(length=work['length'],initial=work['initial'],count=work['count'],reverse=work['reverse'],capacity=work['capacity'],measured=measured,cpu_admitted=len(valid),expected_matches=len(expected),scratch_bytes=work['scratch_bytes'],medians=medians,paired_speedup=ratios))
    checked+=1;attempts+=len(samples)
result=dict(passed=True,input_sha256=hashlib.sha256(a.input.read_bytes()).hexdigest(),cases_checked=checked,recorded_attempts=attempts,independent_scalar_multiplications=targets.cache_info().currsize,oracle='Python hashlib SHA-256, one-based base58 arithmetic, affine secp256k1 test model, OpenSSL RIPEMD160; every admitted scalar checked',cases=rows)
a.output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='cases'},indent=2))
