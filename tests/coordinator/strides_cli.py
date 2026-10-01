#!/usr/bin/env python3
"""Four scalar stride families through real HTTPS and disconnected courier grants."""
import argparse,hashlib,json,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from coordinator_local import Environment,HOST,REPO
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from stride import targets,relations
from model import N
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
p=argparse.ArgumentParser()
for name in ('coordinator','worker','keyhunt','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--apache-root',default='/');p.add_argument('--hardware',action='store_true')
p.add_argument('--backend',choices=('hip','cuda'),default='hip')
p.add_argument('--order',choices=('forward','reverse'),default='forward')
p.add_argument('--orbit',action='store_true')
p.add_argument('--kernel',choices=('direct','stepped','glv'));a=p.parse_args()
worker=str(a.worker.resolve());keyhunt=str(a.keyhunt.resolve())
report=dict(orbit=a.orbit,order=a.order,selected_kernel=a.kernel,passed=False,hardware=a.hardware,backend=a.backend,oracle_commit=check_source(),cases=[],
            binaries={v.name:hashlib.sha256(v.read_bytes()).hexdigest() for v in (a.coordinator,a.worker,a.keyhunt)})
def command(words,ok=True):
    r=subprocess.run(list(map(str,words)),capture_output=True,text=True,timeout=180)
    assert (r.returncode==0)==ok,(words,r.stdout,r.stderr)
    return json.loads(r.stdout) if ok and r.stdout.strip() else r
with tempfile.TemporaryDirectory(prefix='kh-stride-worker-',dir='/var/tmp') as temporary:
    root=Path(temporary);env=Environment(root/'server',str(a.coordinator.resolve()),a.apache_root)
    try:
        env.initialize();env.start();port=env.port
        alice=env.admin('bootstrap',name='stride owner',certificate=(env.directory/'alice.pem').read_text())
        project=env.admin('project-create',name='stride fixtures',owner=alice['client'])['project']
        credentials=dict(endpoint='https://'+env.authority,ca=str(env.directory/'server-ca.pem'),certificate=str(env.directory/'alice.pem'),
                         key=str(env.directory/'alice.key'),resolve=f'{HOST}:{port}:127.0.0.1')
        def private(name,value):
            path=root/name;path.write_text(json.dumps(value));path.chmod(0o600);return path
        courier=private('courier.json',credentials)
        def api(method,path,body=None,status=200):
            with env.client('alice') as client:
                client.request(method,path,None if body is None else json.dumps(body),{'Content-Type':'application/json'})
                response=client.getresponse();payload=response.read();assert response.status==status,(response.status,payload)
                return json.loads(payload)['value'] if status==200 else json.loads(payload)
        for transport in ('https','file'):
            for mode,tag in [('xpoint',1),('hash160',3),('ethereum',4),('vanity',5)]:
                # A different project makes both transport paths execute their
                # full independent job, while preserving identical job semantics.
                project=env.admin('project-create',name=transport+'-'+mode,owner=alice['client'])['project']
                begin=(1<<128)+3;step=1 if a.order=='reverse' and transport=='https' else (1<<64)+7
                count=34;end=begin+count*step-(2 if step>1 else 0)
                indices=[1,17,18,34];seeds=[begin+((count-i) if a.order=='reverse' else (i-1))*step for i in indices]
                indexed=[(v*count+i,seed,v,seed*pow(L,v//2,N)*(-1 if v%2 else 1)%N)
                         for v in range(6 if a.orbit else 1) for i,seed in zip(indices,seeds)]
                scalars=[row[3] for row in indexed];count*=6 if a.orbit else 1
                width=103 if a.orbit else 17
                public=oracle_run(a.oracle,[f'pub {k:064x}' for k in scalars]);_,canonical=targets(mode,public)
                expected={(i,seed,v,k,canonical[t]) for (i,seed,v,k),pub in zip(indexed,public) for t in relations(mode,pub,canonical)}
                config=b'khsearch'+bytes([(5 if a.order=='reverse' else 4) if a.orbit else (3 if a.order=='reverse' else 2),tag])+bytes(40)+b''.join(v.to_bytes(32,'big') for v in (begin,end,step))
                body=dict(mode=mode,begin=f'0x{1:064x}',end_exclusive=f'0x{count+1:064x}',
                    block_width=f'0x{width:064x}',configuration=config.hex(),targets=''.join(canonical))
                for malformed in (dict(body,end_exclusive=f'0x{count+2:064x}'),dict(body,configuration=(config[:-1]+bytes([2 if step==1 else 1])).hex())):
                    assert not api('POST',f'/api/v1/projects/{project}/jobs',malformed,status=400)['ok']
                job=api('POST',f'/api/v1/projects/{project}/jobs',body)['job']
                path=f'/api/v1/projects/{project}/jobs/{job}';state=root/(transport+'-'+mode)
                jobs=[dict(project=project,job=job,devices=['0'],spares=1,policy='sequential')]
                settings=dict(credentials,jobs=jobs) if transport=='https' else dict(endpoint=credentials['endpoint'],transport='file',jobs=jobs)
                def invoke(action,*words,ok=True):return command([worker,action,'--state-dir',state,*words],ok)
                invoke('configure','--config',private(state.name+'.json',settings))
                def sync():
                    if transport=='https':return invoke('sync')
                    # Export retries preserve exact bytes; importing an already
                    # acknowledged response is idempotent at both checkpoints.
                    index=len(list(root.glob(state.name+'-request-*.json')))
                    request=root/f'{state.name}-request-{index}.json';response=root/f'{state.name}-response-{index}.json'
                    exported=invoke('file-export','--output',request)
                    copy=root/f'{state.name}-retry-{index}.json'
                    assert invoke('file-export','--output',copy)==exported and copy.read_bytes()==request.read_bytes()
                    delivered=command([worker,'file-relay','--config',courier,'--input',request,'--sha256',exported['sha256'],'--output',response])
                    imported=invoke('file-import','--input',response,'--sha256',delivered['sha256'])
                    assert invoke('file-import','--input',response,'--sha256',delivered['sha256'])['duplicate']
                    return imported
                sync();assert api('GET',path+'/status')['assignments']==2
                case=dict(mode=mode,transport=transport)
                if a.hardware:
                    if transport=='file':env.stop()
                    command([sys.executable,REPO/'tools/coordinator_worker.py','--state-dir',state,'--worker',worker,'--keyhunt',keyhunt,
                             '--backend',a.backend,'--kernel',a.kernel or ('direct' if transport=='https' else 'stepped'),'--batch-size','8','--once'])
                    events=[json.loads(v) for v in (state/'execution-0.log').read_text().splitlines()]
                    finished=[v for v in events if v.get('type')=='grant-finish']
                    assert len(finished)==2 and [int(v['computed_candidates'],16) for v in finished]==[width,count-width] and all(v['executor_setups']==1 and v['coordinate_space']==('scalar-orbit-index-v1' if a.orbit else 'scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1') for v in finished)
                    assert finished[0]['cold'] and not finished[1]['cold']
                    if transport=='file':assert not (state/'sync.log').exists()
                    local=command([keyhunt,'checkpoint','results','--state-dir',state,'--project',project,'--job',job])['results']
                    def check(rows):
                        assert all(r['coordinate_space']==('scalar-orbit-index-v1' if a.orbit else 'scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1') for r in rows)
                        assert len(rows)==len(expected) and {(int(r['candidate_index'],16),int(r.get('seed_scalar',r['scalar']),16),r.get('orbit_variant',0),int(r['scalar'],16),r['target_bytes']) for r in rows}==expected
                    check(local)
                    for block in (0,1):
                        saved=command([keyhunt,'state','block','--state-dir',state,'--project',project,'--job',job,'--block',block])
                        assert saved['state']=='finished' and [(int(v['begin'],16),int(v['end_exclusive'],16)) for v in saved['covered']]==[(1+width*block,min(1+width*(block+1),count+1))]
                    assert invoke('status')['outbox_bytes']>0
                    if transport=='file':env.start(port=port)
                    assert not api('GET',path+'/results');sync();remote=api('GET',path+'/results');check(remote)
                    assert invoke('status')['outbox_bytes']==0 and int(api('GET',path+'/status')['finished'],16)==2
                    case.update(local_results=local,server_results=remote,events=events)
                command([keyhunt,'state','check','--state-dir',state]);report['cases'].append(case)
        env.admin('check');report['passed']=True
    except Exception as error:report['error']=repr(error);raise
    finally:
        env.stop();a.report.write_text(json.dumps(report,indent=2)+'\n')
print('PASS four-family stride HTTPS and file transport')
