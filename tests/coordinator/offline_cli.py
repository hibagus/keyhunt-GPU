#!/usr/bin/env python3
"""Manual file exchange over real mTLS, with optional disconnected GPU execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from coordinator_local import Environment, HOST, REPO

GX = '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
GY = '483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8'
ETH_TARGET = '7e5f4552091a69125d5dfcb7b8c2659029395bdf'
HASH_TARGETS = '01751e76e8199196d454941c45d1b3a323f1433bd6' + '0291b24bf9f5288532960ac687abb035127b1d28a5'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--coordinator', type=Path, required=True)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--keyhunt', type=Path, required=True)
    parser.add_argument('--apache-root', default='/')
    parser.add_argument('--hardware', action='store_true')
    parser.add_argument('--backend', choices=('hip', 'cuda'), default='hip')
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    worker, keyhunt = str(args.worker.resolve()), str(args.keyhunt.resolve())
    report = dict(passed=False, hardware=args.hardware, backend=args.backend,
                  device=args.device, commands=[], cases=[],
                  binaries={str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in (args.coordinator, args.worker, args.keyhunt)})
    with tempfile.TemporaryDirectory(prefix='kh-offline-', dir='/var/tmp') as temporary:
        root = Path(temporary)
        env = Environment(root / 'server', str(args.coordinator.resolve()), args.apache_root)
        try:
            env.initialize()
            env.start()
            port = env.port
            alice = env.admin('bootstrap', name='offline owner', certificate=(env.directory / 'alice.pem').read_text())
            project = env.admin('project-create', name='offline fixtures', owner=alice['client'])['project']
            courier_config = dict(endpoint='https://' + env.authority, ca=str(env.directory / 'server-ca.pem'),
                                  certificate=str(env.directory / 'alice.pem'), key=str(env.directory / 'alice.key'),
                                  resolve=f'{HOST}:{port}:127.0.0.1')

            def private(name, value):
                path = root / name
                path.write_text(json.dumps(value) + '\n')
                path.chmod(0o600)
                return path

            courier = private('courier.json', courier_config)

            def command(words, ok=True, timeout=60):
                result = subprocess.run(list(map(str, words)), capture_output=True, text=True, timeout=timeout)
                report['commands'].append(dict(command=list(map(str, words)), exit_code=result.returncode,
                                               stdout=result.stdout, stderr=result.stderr))
                assert (result.returncode == 0) == ok, (words, result.stdout, result.stderr)
                return json.loads(result.stdout) if ok and result.stdout.strip() else result

            def invoke(state, action, *options, ok=True):
                return command([worker, action, '--state-dir', state, *options], ok)

            def api(method, path, body=None):
                with env.client('alice') as client:
                    client.request(method, path, None if body is None else json.dumps(body), {'Content-Type': 'application/json'})
                    response = client.getresponse()
                    payload = response.read()
                    assert response.status == 200, (response.status, payload)
                    return json.loads(payload)['value']

            def relay(request, response, checksum, config=courier, ok=True):
                return command([worker, 'file-relay', '--config', config, '--input', request,
                                '--sha256', checksum, '--output', response], ok)

            table = root / 'babies.khb'
            metadata = command([keyhunt, 'bsgs-table', 'build', '--m', '257', '--output', table])
            for mode, width in [('xpoint', 512), ('bsgs', 32768), ('hash160', 512), ('ethereum', 512)]:
                configuration = b'khsearch\x01' + (b'\x01' + bytes(40) if mode == 'xpoint' else b'\x03' + bytes(40) if mode == 'hash160' else b'\x04' + bytes(40) if mode == 'ethereum' else
                                b'\x02' + (257).to_bytes(8, 'big') + bytes.fromhex(metadata['checksum']))
                job = api('POST', f'/api/v1/projects/{project}/jobs', dict(
                    mode=mode, begin=f'0x{1:064x}', end_exclusive=f'0x{1 + width * 2:064x}',
                    block_width=f'0x{width:064x}', configuration=configuration.hex(),
                    targets=GX if mode == 'xpoint' else HASH_TARGETS if mode == 'hash160' else ETH_TARGET if mode == 'ethereum' else '04' + GX + GY))['job']
                path = f'/api/v1/projects/{project}/jobs/{job}'
                state = root / mode
                # No key/certificate/CA paths are copied to the disconnected
                # worker. Only its connected courier can authenticate to HTTPS.
                config = private(mode + '-config.json', dict(endpoint=courier_config['endpoint'], transport='file',
                    jobs=[dict(project=project, job=job, devices=['0'], spares=1, policy='sequential')]))
                invoke(state, 'configure', '--config', config)
                invoke(state, 'sync', ok=False)
                invoke(state, 'scheduled-sync', ok=False)
                request, response = root / (mode + '-request.json'), root / (mode + '-response.json')
                exported = invoke(state, 'file-export', '--output', request)
                copied = root / (mode + '-copy.json')
                assert invoke(state, 'file-export', '--output', copied) == exported
                assert copied.read_bytes() == request.read_bytes()
                invoke(state, 'file-export', '--output', request, ok=False)
                assert api('GET', path + '/status')['assignments'] == 0
                relay(request, response, '0' * 64, ok=False)
                assert not response.exists()
                mismatch = private(mode + '-wrong-authority.json', dict(courier_config, endpoint='https://wrong.invalid'))
                relay(request, response, exported['sha256'], config=mismatch, ok=False)
                wrong_ca = private(mode + '-wrong-ca.json', dict(courier_config, ca=str(env.directory / 'wrong-ca.pem')))
                relay(request, response, exported['sha256'], config=wrong_ca, ok=False)
                assert not response.exists(), 'transport failure published a false acknowledgment'
                delivered = relay(request, response, exported['sha256'])
                assert delivered['status'] == 200 and api('GET', path + '/status')['assignments'] == 2
                assert invoke(state, 'status')['queues'] == [], 'unimported grants executed'
                invoke(state, 'file-import', '--input', response, '--sha256', '0' * 64, ok=False)
                imported = invoke(state, 'file-import', '--input', response, '--sha256', delivered['sha256'])
                assert imported['applied'] and len(imported['worker']['queues']) == 2
                assert invoke(state, 'file-import', '--input', response, '--sha256', delivered['sha256'])['duplicate']
                original_bytes = response.read_bytes()
                relay(request, response, exported['sha256'], ok=False)
                assert response.read_bytes() == original_bytes, 'relay replaced an existing response'

                # The trusted courier can deliver a current authorization refusal.
                # Applying it is durable, yet retains the unresolved request.
                deny_request, deny_response = root / (mode + '-deny-request.json'), root / (mode + '-deny-response.json')
                deny_export = invoke(state, 'file-export', '--output', deny_request)
                env.admin('credential-set', fingerprint=alice['fingerprint'], enabled=False)
                denial = relay(deny_request, deny_response, deny_export['sha256'])
                assert denial['status'] == 401
                denied = invoke(state, 'file-import', '--input', deny_response, '--sha256', denial['sha256'])
                assert denied['worker']['pause_reason'] and denied['worker']['pending_request']
                assert all(row['activity'] == 'server-paused' for row in denied['worker']['queues'])
                env.admin('credential-set', fingerprint=alice['fingerprint'], enabled=True)
                reviewed_request, reviewed_response = root / (mode + '-review-request.json'), root / (mode + '-review-response.json')
                reviewed = invoke(state, 'file-export', '--output', reviewed_request)
                received = relay(reviewed_request, reviewed_response, reviewed['sha256'])
                invoke(state, 'file-import', '--input', reviewed_response, '--sha256', received['sha256'])
                before = invoke(state, 'status')
                assert 'pause_reason' not in before and before['sync_due_in'] is None

                case = dict(mode=mode, reserved_blocks=2, response_sha256=delivered['sha256'])
                if args.hardware:
                    # Stop both coordinator and Apache for actual disconnected
                    # execution. The supervisor must not create a sync child/log.
                    env.stop()
                    words = [sys.executable, REPO / 'tools/coordinator_worker.py', '--state-dir', state,
                             '--worker', worker, '--keyhunt', keyhunt, '--backend', args.backend,
                             '--device-map', f'0={args.device}', '--once']
                    if mode == 'bsgs':
                        words += ['--table', table]
                    command(words, timeout=180)
                    local = invoke(state, 'status')
                    assert local['outbox_bytes'] > 0 and local['last_ack_server_time'] == before['last_ack_server_time']
                    assert all(row['activity'] == 'local-complete-awaiting-sync' for row in local['queues'])
                    assert not (state / 'sync.log').exists(), 'offline supervisor attempted a contact'
                    saved = json.loads((state / 'supervisor.json').read_text())
                    assert saved['devices']['0']['completed'] == 2
                    events = [json.loads(line) for line in (state / 'execution-0.log').read_text().splitlines()]
                    finished = [event for event in events if event.get('type') == 'grant-finish']
                    assert len(finished) == 2 and all(event['executor_setups'] == 1 for event in finished)
                    # Local journal evidence is inspected independently of stdout.
                    blocks = []
                    for block in range(2):
                        inspected = command([keyhunt, 'state', 'block', '--state-dir', state,
                                             '--project', project, '--job', job, '--block', str(block)])
                        assert inspected['state'] == 'finished' and not inspected['remaining']
                        assert [(int(row['begin'], 16), int(row['end_exclusive'], 16)) for row in inspected['covered']] == [
                            (1 + width * block, 1 + width * (block + 1))]
                        blocks.append(inspected)
                    results = command([keyhunt, 'checkpoint', 'results', '--state-dir', state, '--project', project, '--job', job])
                    assert len(results['results']) == (2 if mode == 'hash160' else 1)
                    assert all(int(row['scalar'],16)==1 for row in results['results'])
                    if mode=='hash160':
                        assert {row['target_bytes'] for row in results['results']}=={HASH_TARGETS[:42],HASH_TARGETS[42:]}
                    if mode=='ethereum':
                        assert results['results'][0]['target_bytes']==ETH_TARGET
                    env.start(port=port)
                    assert api('GET', path + '/status')['finished'] == f'0x{0:064x}'
                    upload, acknowledgment = root / (mode + '-upload.json'), root / (mode + '-ack.json')
                    sent = invoke(state, 'file-export', '--output', upload)
                    ack = relay(upload, acknowledgment, sent['sha256'])
                    final = invoke(state, 'file-import', '--input', acknowledgment, '--sha256', ack['sha256'])
                    assert final['worker']['outbox_bytes'] == 0
                    assert all(row['activity'] == 'server-acknowledged' for row in final['worker']['queues'])
                    assert api('GET', path + '/status')['finished'] == f'0x{2:064x}'
                    remote_results = api('GET', path + '/results')
                    assert len(remote_results) == (2 if mode == 'hash160' else 1)
                    assert all(int(row['scalar'],16)==1 for row in remote_results)
                    assert invoke(state, 'file-import', '--input', acknowledgment, '--sha256', ack['sha256'])['duplicate']
                    case.update(local_status=local, final_status=final['worker'], blocks=blocks,
                                local_results=results, server_results=remote_results, events=events)
                command([keyhunt, 'state', 'check', '--state-dir', state])
                report['cases'].append(case)
            env.admin('check')
            report['passed'] = True
        except Exception as error:
            report['error'] = repr(error)
            print(report['error'], file=sys.stderr)
        finally:
            env.stop()
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS' if report['passed'] else 'FAIL') + ' offline CLI / localhost mTLS' +
          (f' / {args.backend} disconnected xpoint, BSGS and HASH160' if args.hardware else ' / CPU transport'))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
