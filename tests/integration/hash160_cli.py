#!/usr/bin/env python3
"""Compare exact bounded native HASH160 searches to independent public keys/hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'oracle'))
from model import N
from oracle_selftest import check_source, run as oracle_run
from hash160 import hash160, address

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--oracle', type=Path, required=True)
parser.add_argument('--report', type=Path, required=True)
parser.add_argument('--hardware', action='store_true')
parser.add_argument('--kernel', choices=['direct', 'stepped'], default='stepped')
parser.add_argument('--backend', choices=['hip', 'cuda'], default='hip')
args = parser.parse_args()
binary = str(args.binary.resolve())
report = {'oracle_commit': check_source(), 'kernel': args.kernel, 'cases': [], 'rejections': 0}

def run(mode, words):
    return subprocess.run([binary, mode, *words], text=True, capture_output=True, timeout=120)

with tempfile.TemporaryDirectory(prefix='keyhunt-c23-') as directory:
    target_file = Path(directory) / 'targets.txt'
    target_file.write_text('00' * 20 + '\n')
    base = ['--backend', args.backend, '--range', '1:2', '--targets', str(target_file)]
    invalid = [[], base + ['--encoding', 'invalid'], base + ['--device', '-1'], base + ['--kernel', 'invalid'],
               base + ['--batch-size', '0'], base + ['--batch-size', '1048577'], base + ['--candidate-capacity', '0'],
               base + ['--candidate-capacity', '1'], base + ['--candidate-capacity', '1048577'],
               base + ['--encoding', 'both', '--encoding', 'compressed'], base + ['--unknown', '1']]
    invalid += [['--backend', args.backend, '--range', r, '--targets', str(target_file)]
                for r in ('0:2', '2:2', '3:2', '1', '1:2:3', f'1:{N+1:x}')]
    for mode in ('hash160', 'address'):
        for words in invalid:
            result = run(mode, words)
            assert result.returncode == 2 and not result.stdout, result
            report['rejections'] += 1
    if not args.hardware:
        for mode in ('hash160', 'address'):
            result = run(mode, base)
            assert result.returncode == 2 and 'not built' in result.stderr and not result.stdout, result
    else:
        for mode, inputs in (
            ('hash160', ('', '0', 'zz'*20, '00'*21, '0'*40+'\0', 'a'*100000)),
            ('address', ('', '1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMJ', '3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy',
                         'mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJRfn', 'bc1qtest', '0OIl'))):
            for data in inputs:
                target_file.write_text(data)
                result = run(mode, base)
                assert result.returncode == 2 and not result.stdout, result
                report['rejections'] += 1
        inventory = json.loads(subprocess.check_output([binary, 'devices', '--backend', args.backend], text=True))
        report['inventory'] = inventory
        # Both encodings, scalar carries and workgroup/group tails are exhaustive
        # over each small interval. Overflow attempts must not advance the cursor.
        cases = [(f'dense_{n}', (1<<80)-3, n, 'dense', 'both', 2049, 4098, 0, 'hash160')
                 for n in (1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025)]
        cases += [(f'carry_{bit}', (1<<bit)-5, 19, 'boundary', 'both', 7, 2, 0, 'hash160')
                  for bit in (32,64,128,192,255)]
        cases += [('low',1,19,'boundary','compressed',8,1,0,'address'),
                  ('order',N-33,33,'dense','both',33,2,0,'address'),
                  ('last',N-1,1,'dense','uncompressed',1,1,0,'hash160'),
                  ('no_match',1<<200,129,'none','both',128,2,0,'hash160'),
                  ('dense_replay',1<<128,129,'dense','both',129,2,0,'hash160'),
                  ('dense_prefix',1<<128,2049,'prefix','both',256,2,0,'hash160')]
        rng = random.Random(0xC23)
        for i in range(8):
            cases.append((f'random_{i}', rng.randrange(2,N-257), rng.randrange(1,65), 'boundary',
                          'compressed' if i%2 else 'uncompressed',17,2,0,'address' if i%2 else 'hash160'))
        for device in inventory['devices']:
            cases.append((f"device_{device['ordinal']}",(1<<160)-7,129,'boundary','both',129,6,device['ordinal'],'hash160'))
        scalars = sorted({k for _,begin,count,*_ in cases for k in range(max(1,begin-1),min(N,begin+count+1))})
        public = dict(zip(scalars,oracle_run(args.oracle,[f'pub {k:064x}' for k in scalars])))
        hashes = {(k,tag): hash160(pub,tag) for k,pub in public.items() for tag in (1,2)}
        for name,begin,count,kind,encoding,batch,capacity,device,mode in cases:
            tags = (1,2) if encoding == 'both' else (1,) if encoding == 'compressed' else (2,)
            selected = range(begin,begin+count) if kind == 'dense' else range(begin,begin+4) if kind == 'prefix' else (
                begin, begin+count//2, begin+count-1, max(1,begin-1), min(N-1,begin+count))
            raw = ['00'*20] if kind == 'none' else [hashes[k,tag] for k in selected for tag in tags]
            targets = sorted({(tag,digest) for digest in raw for tag in tags})
            index = {target:i for i,target in enumerate(targets)}
            expected = [(k,tag,hashes[k,tag],index[tag,hashes[k,tag]]) for k in range(begin,begin+count)
                        for tag in tags if (tag,hashes[k,tag]) in index]
            lines = raw if mode == 'hash160' else [address(digest) for digest in raw]
            target_file.write_text('\r\n'.join(list(reversed(lines))+[lines[0]])+'\r\n\r\n')
            result = run(mode,['--backend',args.backend,'--range',f'{begin:x}:{begin+count:x}','--targets',str(target_file),
                              '--encoding',encoding,'--batch-size',str(batch),'--candidate-capacity',str(capacity),
                              '--device',str(device),'--kernel',args.kernel])
            assert result.returncode == 0 and not result.stderr, (name,result)
            records = [json.loads(line) for line in result.stdout.splitlines()]
            assert records[0]['type']=='start' and records[0]['mode']=='hash160' and records[0]['target_count']==len(targets)
            canonical = b'hash160-v1\0'+b''.join(bytes([tag])+bytes.fromhex(h) for tag,h in targets)
            assert records[0]['target_digest']==hashlib.sha256(canonical).hexdigest()
            cursor=begin; actual=[]; executed=0; overflows=0
            for record in records[1:-1]:
                assert record['type']=='batch' and int(record['begin'],16)==cursor
                assert record['device_steps']==int(record['end_exclusive'],16)-cursor
                executed += record['device_steps']
                if record['overflow']:
                    assert record['verified_steps']==0 and not record['matches'] and record['candidate_count']>capacity
                    overflows += 1
                else:
                    assert record['verified_steps']==record['device_steps']
                    actual += [(int(m['scalar'],16),1 if m['encoding']=='compressed' else 2,m['hash160'],m['target']) for m in record['matches']]
                    cursor=int(record['end_exclusive'],16)
            summary=records[-1]
            assert cursor==begin+count and actual==expected, (name,actual,expected)
            assert summary['complete'] and not summary['durable_coverage']
            assert int(summary['verified_steps'],16)==count and int(summary['device_steps'],16)==executed
            assert int(summary['matches'],16)==len(expected) and summary['launch_count']==len(records)-2
            assert summary['overflow_replays']==overflows
            if name in ('dense_replay','order','dense_prefix'): assert overflows>0
            if name=='dense_prefix': assert summary['launch_count']<40
            report['cases'].append({'name':name,'device':device,'begin':hex(begin),'count':count,
                                    'mode':mode,'encoding':encoding,'targets':len(targets),'summary':summary})
        # Cover all 20 cached offset bits at the maximum legal batch size.
        # These independent fixtures check each intended hit, while the executor
        # still bounds output by two per scalar, never by presumed preimage count.
        begin=1<<200; count=1048576
        offsets=sorted({0,count-1,*[offset for bit in range(20)
                       for offset in ((1<<bit)-1,1<<bit,(1<<bit)+1) if offset<count]})
        pubs=oracle_run(args.oracle,[f'pub {begin+offset:064x}' for offset in offsets])
        digests=[hash160(pub,1) for pub in pubs]
        target_file.write_text('\n'.join(digests))
        result=run('hash160',['--backend',args.backend,'--range',f'{begin:x}:{begin+count:x}',
            '--targets',str(target_file),'--encoding','compressed','--batch-size',str(count),
            '--candidate-capacity',str(len(digests)),'--kernel',args.kernel])
        assert result.returncode==0 and not result.stderr, result
        records=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(records)==3 and records[1]['verified_steps']==count and not records[1]['overflow']
        assert [(int(m['scalar'],16),m['hash160']) for m in records[1]['matches']]==list(zip([begin+o for o in offsets],digests))
        report['cases'].append({'name':'maximum_batch_offset_bits','count':count,'summary':records[-1]})
report['binary_sha256']=hashlib.sha256(args.binary.read_bytes()).hexdigest()
args.report.write_text(json.dumps(report,indent=2)+'\n')
print(f"HASH160 CLI: {len(report['cases'])} independent search cases, {report['rejections']} rejections")
