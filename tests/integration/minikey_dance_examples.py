#!/usr/bin/env python3
"""Run the documented dance minikey commands verbatim."""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--backend',choices=('cpu','hip','cuda'),required=True)
a=p.parse_args();binary=a.binary.resolve();document=ROOT/'docs/C23_MINIKEYS_DANCE.md'
blocks=re.findall(r'<!-- minikey-dance-example: (\w+) -->\n```bash\n(.*?)\n```',document.read_text(),re.S)
assert [name for name,_ in blocks]==['prepare','execute']
report=dict(passed=False,backend=a.backend,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),document_sha256=hashlib.sha256(document.read_bytes()).hexdigest(),artifacts={})
with tempfile.TemporaryDirectory(prefix='kh-minikey-dance-doc-',dir='/var/tmp') as temporary:
 env=dict(os.environ,KEYHUNT_BIN=str(binary),GPU_BACKEND=a.backend,EXAMPLE_PARENT=temporary)
 result=subprocess.run(['bash','-c','\n'.join(code for name,code in blocks if a.backend!='cpu' or name=='prepare')],env=env,cwd=ROOT,text=True,capture_output=True,timeout=120)
 report.update(exit_code=result.returncode,stderr=result.stderr)
 try:
    assert result.returncode==0,result.stderr
    directory=next(Path(temporary).glob('keyhunt-minikey-dance.*'))
    for path in directory.iterdir():
      if path.suffix=='.json':report['artifacts'][path.name]=json.loads(path.read_text())
      if path.suffix=='.ndjson':report['artifacts'][path.name]=[json.loads(line) for line in path.read_text().splitlines()]
    data=report['artifacts'];ordinal=int(data['inspect.json']['ordinal'],16);grant=data['grant.json']['assignments'][0]
    assert int(grant['begin'],16)==ordinal-100 and int(grant['end_exclusive'],16)==ordinal+1
    if a.backend!='cpu':
      batches=data['volatile.ndjson'][1:-1];pivot=ordinal-50
      remaining=set(range(ordinal-100,ordinal+1))
      for index,batch in enumerate(batches):
        lo,hi=int(batch['begin'],16),int(batch['end_exclusive'],16)
        assert not batch['overflow'] and not lo<pivot<hi
        for at in (range(hi-1,lo-1,-1) if index%3==1 else range(lo,hi)):
            above=[v for v in remaining if v>=pivot]
            expected=max(remaining) if index%3==1 else min(above) if index%3==2 and above else min(remaining)
            assert at==expected;remaining.remove(at)
      assert not remaining and len(batches)==6
      assert not batches[0]['matches'] and len(batches[1]['matches'])==1 and int(batches[1]['matches'][0]['ordinal'],16)==ordinal
      rows=data['results.json']['results'];assert len(rows)==1 and int(rows[0]['ordinal'],16)==ordinal
      assert rows[0]['minikey']=='S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy' and rows[0]['scalar']==data['inspect.json']['scalar']
      for name,order in (('volatile','dance'),('durable','dance'),('retry','reverse')):
        summary=data[name+'.ndjson'][-1];assert summary['complete'] and summary['ordinal_order']==order
      assert int(data['durable.ndjson'][-1]['computed_ordinals'],16)==101
      assert data['retry.ndjson'][-1]['batches']==0 and int(data['retry.ndjson'][-1]['resumed_ordinals'],16)==101
      assert data['check.json']['integrity']=='ok'
    report['passed']=True
 except Exception as error:report['error']=repr(error)
a.report.write_text(json.dumps(report,indent=2)+'\n');print('PASS minikey dance example' if report['passed'] else json.dumps(report));raise SystemExit(not report['passed'])
