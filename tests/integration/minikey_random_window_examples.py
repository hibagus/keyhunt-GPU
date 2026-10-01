#!/usr/bin/env python3
"""Run the documented random-window minikey commands verbatim."""
import argparse,hashlib,json,os,re,subprocess,tempfile,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/oracle'))
from minikey_random_window import RandomWindow
p=argparse.ArgumentParser()
for name in ('binary','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--backend',choices=('cpu','hip','cuda'),required=True)
a=p.parse_args();binary=a.binary.resolve();document=ROOT/'docs/C23_MINIKEYS_RANDOM_WINDOW.md'
blocks=re.findall(r'<!-- minikey-random-window-example: (\w+) -->\n```bash\n(.*?)\n```',document.read_text(),re.S)
assert [name for name,_ in blocks]==['prepare','execute']
report=dict(passed=False,backend=a.backend,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),document_sha256=hashlib.sha256(document.read_bytes()).hexdigest(),artifacts={})
with tempfile.TemporaryDirectory(prefix='kh-minikey-random-window-doc-',dir='/var/tmp') as temporary:
 env=dict(os.environ,KEYHUNT_BIN=str(binary),GPU_BACKEND=a.backend,EXAMPLE_PARENT=temporary)
 result=subprocess.run(['bash','-c','\n'.join(code for name,code in blocks if a.backend!='cpu' or name=='prepare')],env=env,cwd=ROOT,text=True,capture_output=True,timeout=120)
 report.update(exit_code=result.returncode,stderr=result.stderr)
 try:
    assert result.returncode==0,result.stderr
    directory=next(Path(temporary).glob('keyhunt-minikey-random-window.*'))
    for path in directory.iterdir():
      if path.suffix=='.json':report['artifacts'][path.name]=json.loads(path.read_text())
      if path.suffix=='.ndjson':report['artifacts'][path.name]=[json.loads(line) for line in path.read_text().splitlines()]
    data=report['artifacts'];ordinal=int(data['inspect.json']['ordinal'],16);grant=data['grant.json']['assignments'][0]
    assert int(grant['begin'],16)==ordinal-100 and int(grant['end_exclusive'],16)==ordinal+1
    if a.backend!='cpu':
      batches=data['volatile.ndjson'][1:-1]
      model=RandomWindow([(ordinal-100,ordinal+1)],42,4)
      for batch in batches:
        planned=model.plan(17,17);assert planned is not None
        assert (int(batch['begin'],16),int(batch['end_exclusive'],16))==planned[:2] and not batch['overflow']
        model.accept()
      assert model.plan(17,17) is None and len(batches)==6
      assert sum(len(batch['matches']) for batch in batches)==1
      rows=data['results.json']['results'];assert len(rows)==1 and int(rows[0]['ordinal'],16)==ordinal
      assert rows[0]['minikey']=='S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy' and rows[0]['scalar']==data['inspect.json']['scalar']
      for name,order in (('volatile','random-window'),('durable','random-window'),('retry','reverse')):
        summary=data[name+'.ndjson'][-1];assert summary['complete'] and summary['ordinal_order']==order
        if order=='random-window':assert int(summary['ordinal_seed'],16)==42 and summary['ordinal_window']==4
      assert int(data['durable.ndjson'][-1]['computed_ordinals'],16)==101
      assert data['retry.ndjson'][-1]['batches']==0 and int(data['retry.ndjson'][-1]['resumed_ordinals'],16)==101
      assert data['check.json']['integrity']=='ok'
    report['passed']=True
 except Exception as error:report['error']=repr(error)
a.report.write_text(json.dumps(report,indent=2)+'\n');print('PASS minikey random-window example' if report['passed'] else json.dumps(report));raise SystemExit(not report['passed'])
