#!/usr/bin/env python3
"""Exercise validated BSGS uploads on every currently visible HIP device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--report',type=Path,required=True)
p.add_argument('--hip',action='store_true')
a=p.parse_args()
binary=str(a.binary.resolve())
report={'devices':[],'rejections':0,'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest()}
def run(words,env=None): return subprocess.run([binary,'bsgs-table',*map(str,words)],text=True,capture_output=True,timeout=60,env=env)
with tempfile.TemporaryDirectory(prefix='keyhunt-c10-') as directory:
 path=Path(directory)/'babies.khb'
 r=run(['build','--m',257,'--output',path]); assert r.returncode==0,r
 base=['validate','--backend','hip','--input',path]
 for words in [base+['--device',-1],base+['--device',2147483648],base+['--max-queries',0],base+['--max-queries',65537],
               ['validate','--backend','cuda','--input',path],base+['--max-queries',1,'--max-queries',2]]:
  r=run(words); assert r.returncode==2 and not r.stdout,r
  report['rejections']+=1
 if not a.hip:
  r=run(base); assert r.returncode==2 and 'not built' in r.stderr and not r.stdout,r
  report['rejections']+=1
 else:
  inventory=json.loads(subprocess.check_output([binary,'devices','--backend','hip'],text=True,timeout=30))
  report['inventory']=inventory
  assert inventory['devices']
  for device in inventory['devices']:
   r=run(base+['--device',device['ordinal'],'--max-queries',129]); assert r.returncode==0 and not r.stderr,r
   result=json.loads(r.stdout); assert result['device_queries']==514 and not result['search_coverage']
   report['devices'].append({'ordinal':device['ordinal'],'uuid':device['uuid'],'result':result})
  for words,env in [(base+['--device',2147483647],None),(base+['--reserve-bytes',(1<<64)-1],None),
                    (base,dict(os.environ,HIP_VISIBLE_DEVICES='-1',ROCR_VISIBLE_DEVICES='-1'))]:
   r=run(words,env); assert r.returncode==2 and not r.stdout,r
   report['rejections']+=1
  # Rehash a saturated filter: every query reaches exact GPU lookup, including
  # all opposite signs. A positive Bloom result must never become a false match.
  data=bytearray(path.read_bytes()); m,buckets,words=struct.unpack_from('<QQQ',data,40)
  pos=128+8*(buckets+1)+48*m
  data[pos:pos+8*words]=b'\xff'*(8*words); data[-32:]=hashlib.sha256(data[:-32]).digest(); path.write_bytes(data)
  r=run(base+['--max-queries',127]); assert r.returncode==0,r
  report['saturated_filter']=json.loads(r.stdout)
a.report.write_text(json.dumps(report,indent=2)+'\n')
print(f"BSGS preparation: {len(report['devices'])} devices, {report['rejections']} rejections")
