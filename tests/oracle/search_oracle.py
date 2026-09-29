#!/usr/bin/env python3
"""Golden boundary/negative search cases for future executors and BSGS tiles."""
import argparse
import hashlib
import json
from pathlib import Path
from model import P, N, add, multiply, encode
from oracle_selftest import check_source, run

ROOT=Path(__file__).resolve().parents[2]


def h(value):
    return f'0x{value:064x}'


def corpus(oracle):
    cases=[]
    spans=[(1,2),(1,18),(0x1000,0x1013),((1<<80)+31,(1<<80)+50),
           ((1<<192)+7,(1<<192)+28),(N-19,N)]
    scalar_set=set()
    for begin,end in spans:
        scalar_set.update(range(max(1,begin-1),min(N,end+2)))
        scalar_set.update((1,2,N-1))
    scalars=sorted(scalar_set)
    expected=run(oracle,[f'pub {k:064x}' for k in scalars])
    pubs=dict(zip(scalars,expected))
    for scalar,pub in pubs.items():
        if pub!=encode(multiply(scalar)):
            raise RuntimeError('independent scalar oracles disagree')
    for begin,end in spans:
        # Below/start/interior/last/end targets, duplicates, opposite Y signs,
        # exact X suffix mismatch, and entirely negative/empty target sets.
        selected=[begin,end-1,min(end-1,begin+1)]
        selected += [k for k in (begin-1,end) if 0<k<N]
        targets=[{'kind':'public_key','value':pubs[k]} for k in selected]
        targets += [targets[0],{'kind':'xpoint','value':pubs[begin][2:66]}]
        altered=pubs[begin][2:64]+f'{int(pubs[begin][64:66],16)^1:02x}'
        targets += [{'kind':'xpoint','value':altered}]
        # For the final order tail, scalar n-1 has G's X but the opposite Y.
        targets += [{'kind':'public_key','value':pubs[1]}, {'kind':'xpoint','value':pubs[1][2:66]}]
        negative=[{'kind':'public_key','value':'04'+'00'*64}, {'kind':'xpoint','value':'00'*32}]
        for group_name,group in [('boundary',targets),('negative',negative),('empty',[])]:
            matches=[]
            for k in range(begin,end):
                for index,target in enumerate(group):
                    actual=pubs[k][2:66] if target['kind']=='xpoint' else pubs[k]
                    if actual==target['value']:
                        matches.append({'scalar':h(k),'target':index})
            cases.append({'name':f'{group_name}_{begin:x}', 'begin':h(begin),'end':h(end),
                          'targets':group,'matches':matches})
    return {'schema':1,'oracle_commit':check_source(),'cases':cases}


def bsgs_mapping_checks():
    # A mathematical fixture check only. Infinity is a real baby step j=0;
    # reconstruction must reject hits beyond a partial tile and process all
    # duplicate targets. The production BSGS executor is not tested by this loop.
    checks=0
    for begin in (1,0x1000,(1<<80)+11,N-20):
        for span in (1,2,7,13):
            end=begin+span
            for m in (1,2,4,8):
                babies={encode(multiply(j)):j for j in range(m)}
                giant=multiply(m)
                negative_giant=(giant[0],-giant[1]%P)
                start=multiply(begin)
                negative_start=(start[0],-start[1]%P)
                target_scalars=list(range(begin,end))+[end,end+1,begin]
                for scalar in target_scalars:
                    current=add(multiply(scalar),negative_start)
                    found=[]
                    for i in range((span+m-1)//m):
                        j=babies.get(encode(current))
                        if j is not None:
                            candidate=begin+i*m+j
                            if begin<=candidate<end:
                                found.append(candidate)
                        current=add(current,negative_giant)
                    expected=[scalar] if begin<=scalar<end else []
                    if found!=expected:
                        raise RuntimeError(f'BSGS mapping mismatch: {begin,span,m,scalar,found}')
                    checks+=1
    return checks


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--oracle',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--write-vectors',action='store_true')
    options=parser.parse_args()
    vectors=corpus(options.oracle)
    path=ROOT/'tests/oracle/search_vectors.json'
    if not options.write_vectors and json.loads(path.read_text())!=vectors:
        raise RuntimeError('search vectors differ from pinned expectations')
    cases=[]
    for case in vectors['cases']:
        begin,end=int(case['begin'],16),int(case['end'],16)
        for width,work,batch in ((1,1,1),(7,5,3),(32,7,4),((1<<256)-1,256,256)):
            targets=[('x:' if t['kind']=='xpoint' else 'p:')+t['value'] for t in case['targets']]
            command=' '.join([case['begin'],case['end'],h(width),h(work),h(batch),*targets])
            expected=h(end-begin)+''.join(f" {m['scalar']}:{m['target']}" for m in case['matches'])
            cases.append((case['name'],command,expected))
    actual=run(options.binary,[c[1] for c in cases])
    failures=[{'case':name,'expected':e,'actual':a} for (name,_,e),a in zip(cases,actual) if e!=a]
    mapping_checks=bsgs_mapping_checks()
    report={'oracle_commit':vectors['oracle_commit'],'search_cases':len(cases),
            'bsgs_mapping_cases':mapping_checks,'binary_sha256':hashlib.sha256(options.binary.read_bytes()).hexdigest(),
            'failures':failures}
    options.report.write_text(json.dumps(report,indent=2)+'\n')
    if not failures and options.write_vectors:
        path.write_text(json.dumps(vectors,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return bool(failures)


if __name__=='__main__':
    raise SystemExit(main())
