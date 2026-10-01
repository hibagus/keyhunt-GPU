#!/usr/bin/env python3
"""Execute the documented public bsgs-reverse example, without duplicating its commands."""
import argparse,hashlib,json,os,re,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/oracle'))
from bsgs_random_window import RandomWindow
p=argparse.ArgumentParser()
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--backend',choices=('cpu','hip','cuda'),required=True)
p.add_argument('--report',type=Path,required=True)
choice=p.add_mutually_exclusive_group()
choice.add_argument('--both-ends',action='store_true')
choice.add_argument('--dance',action='store_true')
choice.add_argument('--random-window',action='store_true')
a=p.parse_args();binary=a.binary.resolve()
name='bsgs-random-window' if a.random_window else 'bsgs-dance' if a.dance else 'bsgs-both-ends' if a.both_ends else 'bsgs-reverse'
order='random-window' if a.random_window else 'dance' if a.dance else 'both-ends' if a.both_ends else 'reverse'
document=ROOT/('docs/C23_BSGS_RANDOM_WINDOW.md' if a.random_window else 'docs/C23_BSGS_DANCE.md' if a.dance else 'docs/C23_BSGS_BOTH_ENDS.md' if a.both_ends else 'docs/C23_BSGS_REVERSE.md')
blocks=re.findall(rf'<!-- {name}-example: (\w+) -->\n```bash\n(.*?)\n```',document.read_text(),re.S)
assert [name for name,_ in blocks]==['prepare','execute']
selected=[code for name,code in blocks if a.backend!='cpu' or name=='prepare']
report=dict(passed=False,tile_order=order,backend=a.backend,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            document_sha256=hashlib.sha256(document.read_bytes()).hexdigest(),artifacts={})
with tempfile.TemporaryDirectory(prefix='kh-bsgs-reverse-docs-',dir='/var/tmp') as temporary:
    env=dict(os.environ,KEYHUNT_BIN=str(binary),GPU_BACKEND=a.backend,EXAMPLE_PARENT=temporary)
    result=subprocess.run(['bash','-c','\n'.join(selected)],env=env,cwd=ROOT,capture_output=True,text=True,timeout=120)
    report.update(exit_code=result.returncode,stderr=result.stderr)
    try:
        assert result.returncode==0,result.stderr
        directory=next(Path(temporary).glob('keyhunt-'+name+'.*'))
        for path in directory.iterdir():
            if path.suffix=='.json':report['artifacts'][path.name]=json.loads(path.read_text())
            if path.suffix=='.ndjson':report['artifacts'][path.name]=[json.loads(line) for line in path.read_text().splitlines()]
        data=report['artifacts'];grant=data['grant.json']['assignments'][0]
        assert int(grant['begin'],16)==1 and int(grant['end_exclusive'],16)==101
        if a.backend!='cpu':
            batches=[row for row in data['volatile.ndjson'] if row['type']=='batch']
            matches=[m for row in batches for m in row['matches']]
            tiles=[row for row in data['volatile.ndjson'] if row['type']=='tile']
            if a.random_window:
                model=RandomWindow([(1,101)],17,2,42,4);expected=[]
                while (tile:=model.next(34)) is not None:expected.append(tile[:2])
                for artifact in ('volatile','durable'):
                    summary=data[artifact+'.ndjson'][-1];assert int(summary['tile_seed'],16)==42 and summary['tile_window']==4
            else:expected=[(1,35),(67,101),(51,67),(35,51)] if a.dance else [(1,35),(67,101),(35,67)] if a.both_ends else [(67,101),(33,67),(1,33)]
            assert [(int(r['begin'],16),int(r['end_exclusive'],16)) for r in tiles]==expected
            for rows in (matches,data['results.json']['results']):
                assert len(rows)==1 and int(rows[0]['scalar'],16)==1
            for artifact,selected_order in (('volatile',order),('durable',order),('retry','forward')):
                summary=data[artifact+'.ndjson'][-1]
                assert summary['complete'] and summary['tile_order']==selected_order
            assert int(data['volatile.ndjson'][-1]['verified_scalars'],16)==100
            assert int(data['durable.ndjson'][-1]['computed_scalars'],16)==100
            retry=data['retry.ndjson'][-1]
            assert retry['batches']==0 and int(retry['resumed_scalars'],16)==100
            assert data['check.json']['integrity']=='ok'
        report['passed']=True
    except Exception as error:report['error']=repr(error)
a.report.write_text(json.dumps(report,indent=2)+'\n')
print('PASS '+name+' example' if report['passed'] else json.dumps(report))
raise SystemExit(0 if report['passed'] else 1)
