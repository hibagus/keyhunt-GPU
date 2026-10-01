#!/usr/bin/env python3
"""Reverse minikeys across two grants through HTTPS and disconnected file exchange."""
import argparse, hashlib, json, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from coordinator_local import Environment, HOST, REPO
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'oracle'))
from oracle_selftest import check_source, run as oracle_run
from minikey import PUBLIC_KEYS,ordinal,text,scalar
from hash160 import hash160

p = argparse.ArgumentParser(description=__doc__)
for name in ('coordinator', 'worker', 'keyhunt', 'oracle', 'report'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--apache-root', default='/')
p.add_argument('--hardware', action='store_true')
p.add_argument('--backend', choices=('hip', 'cuda'), default='hip')
a = p.parse_args()
worker, keyhunt = str(a.worker.resolve()), str(a.keyhunt.resolve())
report = dict(ordinal_order="reverse", passed=False, hardware=a.hardware, backend=a.backend, oracle_commit=check_source(), cases=[],
              binaries={v.name: hashlib.sha256(v.read_bytes()).hexdigest() for v in (a.coordinator, a.worker, a.keyhunt)})

def command(words, ok=True):
    result = subprocess.run(list(map(str, words)), capture_output=True, text=True, timeout=180)
    assert (result.returncode == 0) == ok, (words, result.stdout, result.stderr)
    return json.loads(result.stdout) if ok and result.stdout.strip() else result

with tempfile.TemporaryDirectory(prefix='kh-minikey-reverse-worker-', dir='/var/tmp') as temporary:
    root = Path(temporary)
    env = Environment(root / 'server', str(a.coordinator.resolve()), a.apache_root)
    try:
        env.initialize(); env.start(); port = env.port
        alice = env.admin('bootstrap', name='Minikey owner', certificate=(env.directory / 'alice.pem').read_text())
        credentials = dict(endpoint='https://' + env.authority, ca=str(env.directory / 'server-ca.pem'),
                           certificate=str(env.directory / 'alice.pem'), key=str(env.directory / 'alice.key'),
                           resolve=f'{HOST}:{port}:127.0.0.1')
        def private(name, value):
            path = root / name; path.write_text(json.dumps(value)); path.chmod(0o600); return path
        courier = private('courier.json', credentials)
        def api(method, path, body=None):
            with env.client('alice') as client:
                client.request(method, path, None if body is None else json.dumps(body), {'Content-Type': 'application/json'})
                response = client.getresponse(); payload = response.read()
                assert response.status == 200, (response.status, payload)
                return json.loads(payload)['value']
        for transport,length in (('https',22),('file',22),('https',30),('file',30)):
            begin,width=ordinal(PUBLIC_KEYS[0 if length==22 else 1]),4097
            valid=[n for n in range(begin,begin+2*width) if scalar(text(n,length))]
            seeds=sorted({valid[0],valid[len(valid)//2],valid[-1]})
            private_scalars=[scalar(text(n,length)) for n in seeds]
            public=oracle_run(a.oracle,[f'pub {k:064x}' for k in private_scalars])
            expected={(n,(bytes([length,tag])+bytes.fromhex(hash160(pub,tag))).hex()) for n,pub in zip(seeds,public) for tag in (1,2)}
            canonical=sorted({target for _,target in expected})
            config=b'khsearch\x01\x06'+bytes(40)
            project = env.admin('project-create', name=transport, owner=alice['client'])['project']
            job = api('POST', f'/api/v1/projects/{project}/jobs', dict(
                mode='minikeys', begin=f'0x{begin:064x}', end_exclusive=f'0x{begin+width*2:064x}',
                block_width=f'0x{width:064x}', configuration=config.hex(), targets=''.join(canonical)))['job']
            path = f'/api/v1/projects/{project}/jobs/{job}'; state = root / (transport+str(length))
            jobs = [dict(project=project, job=job, devices=['0'], spares=1, policy='sequential')]
            settings = dict(credentials, jobs=jobs) if transport == 'https' else dict(endpoint=credentials['endpoint'], transport='file', jobs=jobs)
            def invoke(action, *words, ok=True):
                return command([worker, action, '--state-dir', state, *words], ok)
            invoke('configure', '--config', private(transport + '.json', settings))
            def sync():
                if transport == 'https': return invoke('sync')
                # An unacknowledged export is byte-identical. Importing its
                # acknowledgment twice must not duplicate results or receipts.
                index = len(list(root.glob(transport + '-request-*.json')))
                request = root / f'{transport}-request-{index}.json'
                response = root / f'{transport}-response-{index}.json'
                exported = invoke('file-export', '--output', request)
                copy = root / f'{transport}-retry-{index}.json'
                assert invoke('file-export', '--output', copy) == exported and copy.read_bytes() == request.read_bytes()
                delivered = command([worker, 'file-relay', '--config', courier, '--input', request,
                                     '--sha256', exported['sha256'], '--output', response])
                imported = invoke('file-import', '--input', response, '--sha256', delivered['sha256'])
                assert invoke('file-import', '--input', response, '--sha256', delivered['sha256'])['duplicate']
                return imported
            sync(); assert api('GET', path + '/status')['assignments'] == 2
            case = dict(transport=transport, length=length)
            if a.hardware:
                if transport == 'file': env.stop()
                command([sys.executable, REPO / 'tools/coordinator_worker.py', '--state-dir', state,
                         '--worker', worker, '--keyhunt', keyhunt, '--backend', a.backend,
                         '--ordinal-order', 'reverse', '--batch-size', '129', '--once'])
                events = [json.loads(v) for v in (state / 'execution-0.log').read_text().splitlines()]
                finished = [v for v in events if v.get('type') == 'grant-finish']
                assert len(finished) == 2 and all(v['complete'] and v['ordinal_order'] == 'reverse' and
                    int(v['computed_ordinals'], 16) == width and v['executor_setups'] == 1 for v in finished)
                assert finished[0]['cold'] and not finished[1]['cold']
                # Adaptive work-unit widths may change after a timing sample;
                # their exact descending union must still equal each grant.
                cursor = lower = upper = None; units = []
                for event in events:
                    if event.get('type') == 'grant-start':
                        cursor = upper = int(event['grant']['end_exclusive'], 16)
                        lower = int(event['grant']['begin'], 16); units = []
                    elif event.get('type') == 'work-unit':
                        span = event['interval']; lo, hi = int(span['begin'], 16), int(span['end_exclusive'], 16)
                        assert lower <= lo < hi <= upper
                        assert hi == cursor
                        cursor = lo; units.append((lo, hi))
                    elif event.get('type') == 'grant-finish':
                        assert len(units) >= 2 and units[0][1] - units[0][0] == 129
                        ordered=sorted(units)
                        assert ordered[0][0]==lower and ordered[-1][1]==upper
                        assert all(left[1]==right[0] for left,right in zip(ordered,ordered[1:]))
                if transport == 'file': assert not (state / 'sync.log').exists()
                def check(rows):
                    assert all(v["minikey"]==text(int(v["ordinal"],16),length) and int(v["scalar"],16)==scalar(v["minikey"]) for v in rows)
                    assert len(rows) == len(expected) and {(int(v['ordinal'], 16), v['target_bytes']) for v in rows} == expected
                local = command([keyhunt, 'checkpoint', 'results', '--state-dir', state, '--project', project, '--job', job])['results']
                check(local)
                for block in range(2):
                    saved = command([keyhunt, 'state', 'block', '--state-dir', state, '--project', project, '--job', job, '--block', block])
                    assert saved['state'] == 'finished' and not saved['remaining']
                    assert [(int(v['begin'], 16), int(v['end_exclusive'], 16)) for v in saved['covered']] == [(begin+width*block, begin+width*(block+1))]
                assert invoke('status')['outbox_bytes'] > 0
                if transport == 'file': env.start(port=port)
                assert not api('GET', path + '/results'); sync()
                remote = api('GET', path + '/results'); check(remote)
                assert invoke('status')['outbox_bytes'] == 0 and int(api('GET', path + '/status')['finished'], 16) == 2
                case.update(local_results=local, server_results=remote, events=events)
            command([keyhunt, 'state', 'check', '--state-dir', state]); report['cases'].append(case)
        env.admin('check'); report['passed'] = True
    except Exception as error:
        report['error'] = repr(error); raise
    finally:
        env.stop(); a.report.write_text(json.dumps(report, indent=2) + '\n')
print('PASS reverse BSGS HTTPS and disconnected file transport')
