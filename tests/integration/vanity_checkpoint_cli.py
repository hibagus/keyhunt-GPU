#!/usr/bin/env python3
"""Durable prefix relations, overflow, process death and changed launch geometry."""
import argparse,hashlib,json,select,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from hash160 import hash160,address
from model import N
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',choices=('hip','cuda'),default='hip')
a=p.parse_args();binary=str(a.binary.resolve())
report=dict(passed=False,oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
            hardware=a.hardware,backend=a.backend,cases=[],checks=0)
def canonical(tag,prefix):return (bytes([tag,len(prefix)])+prefix.encode()+bytes(34-len(prefix))).hex()
with tempfile.TemporaryDirectory(prefix='kh-vanity-checkpoint-') as directory:
    root=Path(directory);state=root/'state'
    def command(family,action,*words):return [binary,family,action,'--state-dir',str(state),*map(str,words)]
    def call(family,action,*words,ok=True):
        result=subprocess.run(command(family,action,*words),capture_output=True,text=True,timeout=90)
        assert (result.returncode==0)==ok,(words,result.stdout,result.stderr)
        report['checks']+=1
        return [json.loads(line) for line in result.stdout.splitlines()] if ok else result
    def prepare(label,begin,end,prefixes):
        # Kernel variants deliberately share job semantics; isolate their grants by project.
        project=call('state','project-create','--name',label)[0]['project']
        file=root/(label+'.txt');file.write_text('\n'.join(prefixes))
        words=['--project',project,'--mode','vanity','--range',f'{begin:x}:{end:x}',
               '--block-width',f'{end-begin:x}','--targets',file]
        created=call('checkpoint','create',*words)[0]
        file.write_text('\n'.join(list(reversed(prefixes))+[prefixes[0]]))
        assert call('checkpoint','create',*words)[0]==created
        # Encoding is immutable job identity even when the text file is identical.
        assert call('checkpoint','create',*words,'--encoding','compressed')[0]['job']!=created['job']
        scope=['--project',project,'--job',created['job']]
        grant=call('state','claim',*scope,'--owner','test','--request',label)[0]['assignments'][0]['grant']
        return scope,['--backend',a.backend,'--grant',grant,'--targets',file]
    def results(scope):return call('checkpoint','results',*scope,'--limit','1000')[0]['results']
    def verify(scope,expected):
        rows=results(scope)
        assert len(rows)==len(expected) and {(int(row['scalar'],16),row['target_bytes']) for row in rows}==expected
        block=call('state','block',*scope,'--block','0')[0]
        assert block['state']=='finished' and not block['remaining']
        call('state','check')
    for kernel in ('stepped','direct'):
        for label,begin,count in [('overlap',1,33),('carry',(1<<128)-5,19),('order',N-19,19),('none',100,17)]:
            # The pinned libsecp oracle and Python Base58 encoder generate every
            # address independently; expected results enumerate all relations.
            public=oracle_run(a.oracle,[f'pub {k:064x}' for k in range(begin,begin+count)])
            addresses={(begin+i,tag):address(hash160(pub,tag)) for i,pub in enumerate(public) for tag in (1,2)}
            prefixes=(['1','1B','1b','1Bg','1EH',addresses[1,1],addresses[1,2]] if label=='overlap' else
                      ['1'*34] if label=='none' else list(addresses.values()))
            expected={(k,canonical(tag,prefix)) for (k,tag),value in addresses.items() for prefix in prefixes if value.startswith(prefix)}
            scope,run=prepare(label+'-'+kernel,begin,begin+count,prefixes)
            if not a.hardware:
                rejected=call('checkpoint','run',*run,ok=False)
                assert 'not built' in rejected.stderr and not results(scope)
                continue
            bad=root/'wrong.txt';bad.write_text('12')
            mismatch=run.copy();mismatch[mismatch.index('--targets')+1]=bad
            for words in (mismatch,run+['--encoding','compressed'],run+['--input-format','address'],run+['--candidate-capacity','1']):
                assert not call('checkpoint','run',*words,ok=False).stdout
            bound=2*len({len(v) for v in prefixes})
            summary=call('checkpoint','run',*run,'--batch-size','64','--candidate-capacity',bound,'--kernel',kernel)[-1]
            assert summary['complete'] and int(summary['computed_scalars'],16)==count and summary['durability']=='local'
            if label!='none':assert summary['overflow_replays']>0
            verify(scope,expected)
            again=call('checkpoint','run',*run)[-1]
            assert again['batches']==0 and int(again['resumed_scalars'],16)==count
            report['cases'].append(dict(name=label,kernel=kernel,relations=len(expected),summary=summary))
    if a.hardware:
        seeds=[1,2,1048576]
        public=oracle_run(a.oracle,[f'pub {k:064x}' for k in seeds])
        values={(k,tag):address(hash160(pub,tag)) for k,pub in zip(seeds,public) for tag in (1,2)}
        scope,run=prepare('killed',1,1048577,list(values.values()))
        process=subprocess.Popen(command('checkpoint','run',*run,'--batch-size','32'),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            assert select.select([process.stdout],[],[],30)[0],'no durable acknowledgment'
            notice=json.loads(process.stdout.readline())
            assert notice['type']=='checkpoint' and notice['durable_results'] and notice['durability']=='local'
            assert 'already held' in call('checkpoint','run',*run,ok=False).stderr
            process.kill();process.communicate(timeout=30)
        finally:
            if process.poll() is None:process.kill();process.communicate(timeout=30)
        call('state','check');assert results(scope)
        retained=call('state','block',*scope,'--block','0')[0]
        covered=sum(int(v['end_exclusive'],16)-int(v['begin'],16) for v in retained['covered'])
        summary=call('checkpoint','run',*run,'--batch-size','65536','--kernel','direct')[-1]
        assert summary['complete'] and int(summary['resumed_scalars'],16)==covered
        assert covered+int(summary['computed_scalars'],16)==1048576
        verify(scope,{(k,canonical(tag,value)) for (k,tag),value in values.items()})
        report['cases'].append(dict(name='kill-and-restart',retained=retained,summary=summary))
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
