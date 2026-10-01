#!/usr/bin/env python3
"""Execute the documented public orbit example, without duplicating its commands."""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--backend',choices=('cpu','hip','cuda'),required=True)
p.add_argument('--report',type=Path,required=True)
a=p.parse_args();binary=a.binary.resolve();document=ROOT/'docs/C23_ORBITS.md'
blocks=re.findall(r'<!-- orbit-example: (\w+) -->\n```bash\n(.*?)\n```',document.read_text(),re.S)
assert [name for name,_ in blocks]==['prepare','execute']
selected=[code for name,code in blocks if a.backend!='cpu' or name=='prepare']
report=dict(passed=False,backend=a.backend,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            document_sha256=hashlib.sha256(document.read_bytes()).hexdigest(),artifacts={})
with tempfile.TemporaryDirectory(prefix='kh-orbit-docs-',dir='/var/tmp') as temporary:
    env=dict(os.environ,KEYHUNT_BIN=str(binary),GPU_BACKEND=a.backend,EXAMPLE_PARENT=temporary)
    result=subprocess.run(['bash','-c','\n'.join(selected)],env=env,cwd=ROOT,capture_output=True,text=True,timeout=120)
    report.update(exit_code=result.returncode,stderr=result.stderr)
    try:
        assert result.returncode==0,result.stderr
        directory=next(Path(temporary).glob('keyhunt-orbit.*'))
        for path in directory.iterdir():
            if path.suffix=='.json':report['artifacts'][path.name]=json.loads(path.read_text())
            if path.suffix=='.ndjson':report['artifacts'][path.name]=[json.loads(line) for line in path.read_text().splitlines()]
        data=report['artifacts'];grant=data['grant.json']['assignments'][0]
        assert int(grant['begin'],16)==1 and int(grant['end_exclusive'],16)==19
        if a.backend!='cpu':
            n=int('fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141',16)
            wanted={(3,1,0,1),(6,1,1,n-1)}
            matches=[m for row in data['volatile.ndjson'] for m in row.get('matches',[])]
            for rows in (matches,data['results.json']['results']):
                assert len(rows)==2 and {(int(r['candidate_index'],16),int(r['seed_scalar'],16),r['orbit_variant'],int(r['scalar'],16)) for r in rows}==wanted
            for name in ('volatile','durable','retry'):
                summary=data[name+'.ndjson'][-1]
                assert summary['complete'] and summary['coordinate_space']=='scalar-orbit-index-v1'
            assert int(data['volatile.ndjson'][-1]['verified_steps'],16)==18
            assert int(data['durable.ndjson'][-1]['computed_candidates'],16)==18
            retry=data['retry.ndjson'][-1]
            assert retry['batches']==0 and int(retry['resumed_candidates'],16)==18
            assert data['check.json']['integrity']=='ok'
        report['passed']=True
    except Exception as error:report['error']=repr(error)
a.report.write_text(json.dumps(report,indent=2)+'\n')
print('PASS orbit example' if report['passed'] else json.dumps(report))
raise SystemExit(0 if report['passed'] else 1)
