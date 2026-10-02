#!/usr/bin/env python3
"""Audit file exchange: local coordinator/courier, remote CUDA worker over SSH.

Uses public scalar-1 fixtures and disposable state. Only configuration, request
and response files cross hosts; the fixture credentials stay with the courier.
SSH is the trusted file/control channel, not a simulated physical air gap.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--apache-root', required=True)
    p.add_argument('--host', required=True)
    p.add_argument('--remote-source', required=True)
    p.add_argument('--remote-build', required=True)
    p.add_argument('--device', type=int, default=7)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if sys.flags.optimize:
        raise RuntimeError('Assertions must be enabled')
    sys.path.insert(0, str(a.source.resolve() / 'tools'))
    from coordinator_local import Environment, HOST

    report = dict(passed=False, source=str(a.source), host=a.host, device=a.device,
                  commands=[], transfers=[], cases=[])
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', a.host]

    def command(args, remote=False, decode=True, ok=True):
        words = list(map(str, args))
        cmd = ssh + [shlex.join(words)] if remote else words
        run = subprocess.run(cmd, text=True, capture_output=True, timeout=240)
        report['commands'].append(dict(command=cmd, exit_code=run.returncode,
                                       stdout=run.stdout, stderr=run.stderr))
        assert (run.returncode == 0) == ok, (cmd, run.returncode, run.stderr)
        return json.loads(run.stdout) if decode and ok else run.stdout.strip()

    remote = None
    try:
        remote = command(['mktemp', '-d', '/var/tmp/keyhunt-c22-courier-XXXXXXXX'],
                         remote=True, decode=False)
        assert remote.startswith('/var/tmp/keyhunt-c22-courier-') and '\n' not in remote
        report['remote_directory'] = remote
        report['identities'] = dict(
            local_hostname=command(['hostname'], decode=False),
            remote_hostname=command(['hostname'], remote=True, decode=False),
            local_boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            remote_boot=command(['cat', '/proc/sys/kernel/random/boot_id'], remote=True, decode=False),
            source=command(['git', '-C', a.source, 'rev-parse', 'HEAD'], decode=False),
            remote_source=command(['git', '-C', a.remote_source, 'rev-parse', 'HEAD'], remote=True, decode=False))
        assert report['identities']['local_boot'] != report['identities']['remote_boot']
        assert report['identities']['source'] == report['identities']['remote_source']
        rw = str(Path(a.remote_build) / 'keyhunt-worker')
        rk = str(Path(a.remote_build) / 'keyhunt')
        report['binary_hashes'] = dict(
            local={name: hashlib.sha256((a.build / name).read_bytes()).hexdigest()
                   for name in ('keyhunt-coordinator', 'keyhunt-worker')},
            remote=command(['sha256sum', rw, rk], remote=True, decode=False))
        with tempfile.TemporaryDirectory(prefix='keyhunt-c22-courier-', dir='/var/tmp') as temporary:
            root = Path(temporary)
            env = Environment(root / 'server', str((a.build / 'keyhunt-coordinator').resolve()), a.apache_root)
            try:
                env.initialize()
                env.start()
                port = env.port
                owner = env.admin('bootstrap', name='cross-host audit',
                                  certificate=(env.directory / 'alice.pem').read_text())
                project = env.admin('project-create', name='public fixtures', owner=owner['client'])['project']

                def private(name, data):
                    path = root / name
                    path.write_text(json.dumps(data) + '\n')
                    path.chmod(0o600)
                    return path

                courier = private('courier.json', dict(endpoint='https://' + env.authority,
                    ca=str(env.directory / 'server-ca.pem'), certificate=str(env.directory / 'alice.pem'),
                    key=str(env.directory / 'alice.key'), resolve=f'{HOST}:{port}:127.0.0.1'))

                def api(method, path, body=None):
                    with env.client('alice') as client:
                        client.request(method, path, None if body is None else json.dumps(body),
                                       {'Content-Type': 'application/json'})
                        response = client.getresponse()
                        value = response.read()
                        assert response.status == 200, (response.status, value)
                        return json.loads(value)['value']

                def transfer(local, remote_path, upload):
                    endpoint = a.host + ':' + remote_path
                    words = [str(local), endpoint] if upload else [endpoint, str(local)]
                    command(['scp', '-q', '-o', 'BatchMode=yes', *words], decode=False)
                    local.chmod(0o600)
                    digest = hashlib.sha256(local.read_bytes()).hexdigest()
                    other = command(['sha256sum', remote_path], remote=True, decode=False).split()[0]
                    assert digest == other
                    report['transfers'].append(dict(file=local.name, upload=upload,
                                                    bytes=local.stat().st_size, sha256=digest))

                def worker(state, action, *args, **kwargs):
                    return command([rw, action, '--state-dir', state, *args], remote=True, **kwargs)

                table = remote + '/babies.khb'
                metadata = command([rk, 'bsgs-table', 'build', '--m', '257', '--output', table], remote=True)
                gx = '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
                gy = '483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8'
                for mode, width in [('xpoint', 513), ('bsgs', 32769)]:
                    configuration = b'khsearch\x01' + (b'\x01' + bytes(40) if mode == 'xpoint' else
                        b'\x02' + (257).to_bytes(8, 'big') + bytes.fromhex(metadata['checksum']))
                    job = api('POST', f'/api/v1/projects/{project}/jobs', dict(mode=mode,
                        begin=f'0x{1:064x}', end_exclusive=f'0x{1 + 2 * width:064x}',
                        block_width=f'0x{width:064x}', configuration=configuration.hex(),
                        targets=gx if mode == 'xpoint' else '04' + gx + gy))['job']
                    path = f'/api/v1/projects/{project}/jobs/{job}'
                    state = remote + '/' + mode
                    config = private(mode + '-config.json', dict(transport='file',
                        endpoint='https://' + env.authority,
                        jobs=[dict(project=project, job=job, devices=['0'], spares=1, policy='sequential')]))
                    remote_config = remote + '/' + config.name
                    transfer(config, remote_config, True)
                    worker(state, 'configure', '--config', remote_config)
                    worker(state, 'sync', ok=False)

                    def exchange(label):
                        request = root / f'{mode}-{label}-request.json'
                        response = root / f'{mode}-{label}-response.json'
                        remote_request, remote_response = remote + '/' + request.name, remote + '/' + response.name
                        sent = worker(state, 'file-export', '--output', remote_request)
                        transfer(request, remote_request, False)
                        assert sent['sha256'] == hashlib.sha256(request.read_bytes()).hexdigest()
                        delivered = command([a.build / 'keyhunt-worker', 'file-relay', '--config', courier,
                            '--input', request, '--sha256', sent['sha256'], '--output', response])
                        assert delivered['status'] == 200
                        transfer(response, remote_response, True)
                        result = worker(state, 'file-import', '--input', remote_response, '--sha256', delivered['sha256'])
                        assert result['applied'] and result['status'] == 200
                        assert worker(state, 'file-import', '--input', remote_response,
                                      '--sha256', delivered['sha256'])['duplicate']
                        return result

                    first = exchange('reserve')
                    assert api('GET', path + '/status')['assignments'] == 2
                    env.stop()
                    words = ['python3', Path(a.remote_source) / 'tools/coordinator_worker.py', '--state-dir', state,
                             '--worker', rw, '--keyhunt', rk, '--backend', 'cuda',
                             '--device-map', f'0={a.device}', '--once']
                    if mode == 'bsgs':
                        words += ['--table', table]
                    command(words, remote=True, decode=False)
                    local = worker(state, 'status')
                    assert local['outbox_bytes'] > 0 and local['last_ack_server_time'] == first['worker']['last_ack_server_time']
                    assert all(row['activity'] == 'local-complete-awaiting-sync' for row in local['queues'])
                    assert local['sync_due_in'] is None
                    command(['test', '!', '-e', state + '/sync.log'], remote=True, decode=False)
                    saved = command(['cat', state + '/supervisor.json'], remote=True)
                    assert saved['devices']['0']['completed'] == 2
                    events = [json.loads(line) for line in command(['cat', state + '/execution-0.log'],
                              remote=True, decode=False).splitlines()]
                    finished = [e for e in events if e.get('type') == 'grant-finish']
                    assert len(finished) == 2 and all(e['executor_setups'] == 1 for e in finished)
                    assert [e['cold'] for e in finished] == [True, False]
                    blocks = []
                    for block in range(2):
                        value = command([rk, 'state', 'block', '--state-dir', state,
                            '--project', project, '--job', job, '--block', block], remote=True)
                        assert value['state'] == 'finished' and not value['remaining']
                        assert [(int(row['begin'], 16), int(row['end_exclusive'], 16)) for row in value['covered']] == [
                            (1 + block * width, 1 + (block + 1) * width)]
                        blocks.append(value)
                    results = command([rk, 'checkpoint', 'results', '--state-dir', state,
                                       '--project', project, '--job', job], remote=True)
                    assert len(results['results']) == 1 and int(results['results'][0]['scalar'], 16) == 1
                    env.start(port=port)
                    assert int(api('GET', path + '/status')['finished'], 16) == 0
                    final = exchange('ack')
                    assert final['worker']['outbox_bytes'] == 0
                    assert all(row['activity'] == 'server-acknowledged' for row in final['worker']['queues'])
                    server_status, server_results = api('GET', path + '/status'), api('GET', path + '/results')
                    assert int(server_status['finished'], 16) == 2
                    assert len(server_results) == 1 and int(server_results[0]['scalar'], 16) == 1
                    assert server_results[0]['target_bytes'] == results['results'][0]['target_bytes']
                    command([rk, 'state', 'check', '--state-dir', state], remote=True)
                    report['cases'].append(dict(mode=mode, width=width, reserved_blocks=2,
                        blocks=blocks, events=events, local_status=local, final_status=final['worker'],
                        local_results=results, server_results=server_results, server_status=server_status))
                report['server_check'] = env.admin('check')
                report['passed'] = True
            finally:
                env.stop()
    except Exception as error:
        report['error'] = repr(error)
        print(report['error'], file=sys.stderr)
    finally:
        # A failed SSH invocation can leave an owner alive. Retain its journal
        # for inspection; remove only a successfully drained fixture.
        if report['passed'] and remote is not None:
            try:
                command(['python3', '-c', 'import shutil,sys;shutil.rmtree(sys.argv[1])', remote],
                        remote=True, decode=False)
            except Exception as error:
                report['passed'] = False
                report['cleanup_error'] = repr(error)
        a.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS cross-host courier' if report['passed'] else 'FAIL cross-host courier')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
