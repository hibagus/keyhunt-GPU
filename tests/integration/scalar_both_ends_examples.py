#!/usr/bin/env python3
"""Execute the public scalar example verbatim and check its full batch sequence."""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--backend',choices=('cpu','hip','cuda'),required=True);a=p.parse_args()
document=ROOT/'docs/C23_SCALAR_BOTH_ENDS.md';binary=a.binary.resolve()
blocks=re.findall(r'<!-- scalar-both-ends-example: (\w+) -->\n```bash\n(.*?)\n```',document.read_text(),re.S)
assert [name for name,_ in blocks]==['prepare','execute']
report=dict(passed=False,backend=a.backend,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),document_sha256=hashlib.sha256(document.read_bytes()).hexdigest(),artifacts={})
with tempfile.TemporaryDirectory(prefix='kh-scalar-doc-',dir='/var/tmp') as temporary:
 env=dict(os.environ,KEYHUNT_BIN=str(binary),GPU_BACKEND=a.backend,EXAMPLE_PARENT=temporary)
 result=subprocess.run(['bash','-c','\n'.join(code for name,code in blocks if a.backend!='cpu' or name=='prepare')],env=env,cwd=ROOT,capture_output=True,text=True,timeout=120)
 report.update(exit_code=result.returncode,stderr=result.stderr)
 try:
  assert result.returncode==0,result.stderr
  directory=next(Path(temporary).glob('keyhunt-scalar-both-ends.*'))
  for path in directory.iterdir():
   if path.suffix=='.json':report['artifacts'][path.name]=json.loads(path.read_text())
   if path.suffix=='.ndjson':report['artifacts'][path.name]=[json.loads(line) for line in path.read_text().splitlines()]
  data=report['artifacts'];grant=data['grant.json']['assignments'][0]
  assert int(grant['begin'],16)==1 and int(grant['end_exclusive'],16)==102
  if a.backend!='cpu':
   batches=data['volatile.ndjson'][1:-1]
   assert [(int(b['begin'],16),int(b['end_exclusive'],16)) for b in batches]==[(1,18),(85,102),(18,35),(68,85),(35,52),(52,68)]
   assert all(not b['overflow'] for b in batches)
   matches=[m for b in batches for m in b['matches']];assert len(matches)==1 and int(matches[0]['scalar'],16)==1
   rows=data['results.json']['results'];assert len(rows)==1 and int(rows[0]['scalar'],16)==1
   for name,order in [('volatile','both-ends'),('durable','both-ends'),('retry','forward')]:
    assert data[name+'.ndjson'][-1]['complete'] and data[name+'.ndjson'][-1]['batch_order']==order
   assert int(data['durable.ndjson'][-1]['computed_scalars'],16)==101
   assert data['retry.ndjson'][-1]['batches']==0 and int(data['retry.ndjson'][-1]['resumed_scalars'],16)==101
   assert data['check.json']['integrity']=='ok'
  report['passed']=True
 except Exception as error:report['error']=repr(error)
a.report.write_text(json.dumps(report,indent=2)+'\n');print('PASS scalar both-ends example' if report['passed'] else json.dumps(report));raise SystemExit(not report['passed'])
