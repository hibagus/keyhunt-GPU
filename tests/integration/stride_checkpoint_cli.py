#!/usr/bin/env python3
"""Exact candidate-index checkpoints with independent four-family result sets."""
import argparse,hashlib,json,select,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from stride import targets,relations
from model import N
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',choices=('hip','cuda'),default='hip')
p.add_argument('--order',choices=('forward','reverse'),default='forward')
p.add_argument('--orbit',action='store_true')
p.add_argument('--kernel',choices=('direct','stepped','glv'))
a=p.parse_args();binary=str(a.binary.resolve())
report=dict(orbit=a.orbit,order=a.order,selected_kernel=a.kernel,passed=False,hardware=a.hardware,oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),cases=[])
with tempfile.TemporaryDirectory(prefix='kh-stride-checkpoint-') as directory:
    root=Path(directory);state=root/'state'
    def command(family,action,*words):return [binary,family,action,'--state-dir',str(state),*map(str,words)]
    def call(family,action,*words,ok=True):
        r=subprocess.run(command(family,action,*words),capture_output=True,text=True,timeout=120)
        assert (r.returncode==0)==ok,(words,r.stdout,r.stderr)
        return [json.loads(line) for line in r.stdout.splitlines()] if ok else r
    def prepare(mode,label,begin,end,step,indices):
        project=call('state','project-create','--name',mode+'-'+label)[0]['project']
        count=1+(end-begin-1)//step
        seeds=[begin+((count-i) if a.order=='reverse' else (i-1))*step for i in indices]
        coordinates=[(v*count+i,seed,v,seed*pow(L,v//2,N)*(-1 if v%2 else 1)%N)
                     for v in range(6 if a.orbit else 1) for i,seed in zip(indices,seeds)]
        indices=[row[0] for row in coordinates];scalars=[row[3] for row in coordinates]
        public=oracle_run(a.oracle,[f'pub {k:064x}' for k in scalars])
        lines,canonical=targets(mode,public,overlap=label.startswith('overlap') and mode=='vanity')
        file=root/'targets.txt';file.write_text('\n'.join(lines))
        count=(6 if a.orbit else 1)*(1+(end-begin-1)//step)
        words=['--project',project,'--mode',mode,'--range',f'{begin:x}:{end:x}','--stride',f'{step:x}',
               '--block-width',f'{count:x}','--targets',file]
        if a.orbit:words+=['--endomorphism','orbit']
        if a.order=='reverse':words+=['--order','reverse']
        created=call('checkpoint','create',*words)[0]
        file.write_text('\n'.join(reversed(lines))+ '\n'+lines[0])
        assert call('checkpoint','create',*words)[0]==created
        scope=['--project',project,'--job',created['job']]
        grant=call('state','claim',*scope,'--owner','test','--request','claim')[0]['assignments'][0]['grant']
        expected={(i,seed,v,k,canonical[t]) for (i,seed,v,k),pub in zip(coordinates,public) for t in relations(mode,pub,canonical)}
        bound=2*len({len(v) for v in lines}) if mode=='vanity' else 2 if mode=='hash160' else 1
        return scope,['--backend',a.backend,'--grant',grant,'--targets',file],expected,bound
    def verify(scope,expected):
        rows=call('checkpoint','results',*scope,'--limit','1000')[0]['results']
        assert all(row['coordinate_space']==('scalar-orbit-index-v1' if a.orbit else 'scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1') for row in rows)
        assert len(rows)==len(expected) and {(int(r['candidate_index'],16),int(r.get('seed_scalar',r['scalar']),16),r.get('orbit_variant',0),int(r['scalar'],16),r['target_bytes']) for r in rows}==expected
        assert call('state','block',*scope,'--block','0')[0]['state']=='finished'
        call('state','check')
    for mode in ('xpoint','hash160','ethereum','vanity'):
        for kernel in ([a.kernel] if a.kernel else ('direct','stepped')):
            for label,begin,end,step in ([('unit',101,108,1),('wide',1<<128,(1<<128)+7*((1<<192)+1),(1<<192)+1),('overlapping-orbits',1,N,N-2)] if a.orbit else [('overlap',101,101+33*7-3,7),('wide',1<<128,(1<<128)+17*((1<<192)+1),(1<<192)+1),('order',N-129,N,7)]+([('unit',101,134,1)] if a.order=='reverse' else [])):
                count=1+(end-begin-1)//step
                scope,run,expected,bound=prepare(mode,label+'-'+kernel,begin,end,step,list(range(1,count+1)))
                count*=6 if a.orbit else 1
                if a.orbit:
                    assert not call('checkpoint','run',*run,'--endomorphism','none',ok=False).stdout
                # Restarts infer the immutable stride. An explicit mismatch must
                # fail before coverage or a result can be accepted.
                rejected=call('checkpoint','run',*run,'--stride','2' if step==1 else '1',ok=False);assert not rejected.stdout
                rejected=call('checkpoint','run',*run,'--order','forward' if a.order=='reverse' else 'reverse',ok=False);assert not rejected.stdout
                if not a.hardware:
                    assert 'not built' in call('checkpoint','run',*run,ok=False).stderr;continue
                summary=call('checkpoint','run',*run,'--batch-size','64','--candidate-capacity',bound,'--kernel',kernel)[-1]
                assert summary['complete'] and int(summary['computed_candidates'],16)==count and (label=='overlapping-orbits' or summary['overflow_replays']>0)
                assert summary['coordinate_space']==('scalar-orbit-index-v1' if a.orbit else 'scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1')
                verify(scope,expected)
                done=call('checkpoint','run',*run,'--stride',f'{step:x}')[-1]
                assert done['batches']==0 and int(done['resumed_candidates'],16)==count
                report['cases'].append(dict(mode=mode,kernel=kernel,name=label,summary=summary))
        if a.hardware:
            begin=1<<100;step=17;count=65537 if a.orbit else 1048576
            scope,run,expected,bound=prepare(mode,'killed',begin,begin+count*step,step,[1,2,count])
            count*=6 if a.orbit else 1
            child=subprocess.Popen(command('checkpoint','run',*run,'--batch-size','32',*(['--kernel',a.kernel or 'glv'] if a.kernel or a.orbit else [])),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                assert select.select([child.stdout],[],[],30)[0],'no durable acknowledgment'
                notice=json.loads(child.stdout.readline());assert notice['coordinate_space']==('scalar-orbit-index-v1' if a.orbit else 'scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1')
                assert 'already held' in call('checkpoint','run',*run,ok=False).stderr
                child.kill();child.communicate(timeout=30)
            finally:
                if child.poll() is None:child.kill();child.communicate(timeout=30)
            saved=call('state','block',*scope,'--block','0')[0]
            covered=sum(int(v['end_exclusive'],16)-int(v['begin'],16) for v in saved['covered'])
            summary=call('checkpoint','run',*run,'--batch-size','65536','--kernel','direct')[-1]
            assert int(summary['resumed_candidates'],16)==covered and covered+int(summary['computed_candidates'],16)==count
            verify(scope,expected);report['cases'].append(dict(mode=mode,name='kill-restart',summary=summary,retained=saved))
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
