#!/usr/bin/env python3
"""Check the pinned upstream source, independent affine model and public vectors."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
from model import P, N, G, add, multiply, encode

ROOT = Path(__file__).resolve().parents[2]
SEED = 0xC06A11CE


def check_source():
    lock = json.loads((ROOT/'tests/oracle/secp256k1.lock.json').read_text())
    vendor = ROOT/'third_party/secp256k1-oracle'
    for name, expected in lock['files'].items():
        if hashlib.sha256((vendor/name).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'pinned oracle source changed: {name}')
    return lock['commit']


def public_vectors():
    scalars = [0, 1, 2, 3, N-2, N-1, N, N+1, (1<<256)-1]
    for bit in (31, 32, 63, 64, 127, 128, 191, 192, 255):
        scalars += [(1<<bit)-1, 1<<bit, (1<<bit)+1]
    return {'schema': 1, 'oracle_commit': check_source(),
            'vectors': [{'scalar': f'{k:064x}',
                         'public_key': encode(multiply(k)) if 0 < k < N else 'invalid'} for k in scalars]}


def run(binary, commands):
    result = subprocess.run([str(binary.resolve())], input='\n'.join(commands)+'\n',
                            capture_output=True, text=True, timeout=90)
    if result.returncode or result.stderr:
        raise RuntimeError(f'probe exited {result.returncode}: {result.stderr[:4000]}')
    lines = result.stdout.splitlines()
    if len(lines) != len(commands):
        raise RuntimeError(f'expected {len(commands)} results, got {len(lines)}')
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--write-vectors', action='store_true')
    options = parser.parse_args()
    vectors = public_vectors()
    path = ROOT/'tests/oracle/vectors.json'
    if not options.write_vectors and json.loads(path.read_text()) != vectors:
        raise RuntimeError('committed vectors differ from independent model/pin')
    cases = [('pub '+v['scalar'], v['public_key']) for v in vectors['vectors']]
    rng = random.Random(SEED)
    points = [None, G, (G[0], P-G[1])]
    for _ in range(128):
        scalar = rng.randrange(1,N)
        point = multiply(scalar)
        points.append(point)
        cases.append((f'pub {scalar:064x}', encode(point)))
        cases.append(('parse '+encode(point,True), encode(point)))
    for point in points:
        negative = None if point is None else (point[0], -point[1] % P)
        cases += [('neg '+encode(point), encode(negative)),
                  ('add '+encode(point)+' '+encode(point), encode(add(point,point))),
                  ('add '+encode(point)+' '+encode(negative), 'inf'),
                  ('add inf '+encode(point), encode(point)),
                  ('add '+encode(point)+' inf', encode(point))]
    for _ in range(128):
        a, b = rng.sample(points,2)
        cases.append(('add '+encode(a)+' '+encode(b), encode(add(a,b))))
    for invalid in ['00','05'+encode(G)[2:66], '02'+f'{P:064x}', '02'+'00'*32,
                    '04'+'00'*64, encode(G)[:-2], '04'+f'{G[0]:064x}{(G[1]+1):064x}']:
        cases.append(('parse '+invalid,'invalid'))
    actual = run(options.binary,[c[0] for c in cases])
    failures = [{'command':c,'expected':e,'actual':a} for (c,e),a in zip(cases,actual) if e!=a]
    report = {'oracle_commit':vectors['oracle_commit'], 'seed':SEED, 'cases':len(cases),
              'binary_sha256':hashlib.sha256(options.binary.read_bytes()).hexdigest(), 'failures':failures[:20]}
    options.report.write_text(json.dumps(report,indent=2)+'\n')
    if not failures and options.write_vectors:
        path.write_text(json.dumps(vectors,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
