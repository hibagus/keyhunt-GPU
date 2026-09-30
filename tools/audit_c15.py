#!/usr/bin/env python3
"""Audit C15 warm HIP rates, supervised block costs, and pause/watchdog behavior.

Uses synthetic public targets, one caller-selected GPU, and the repository's
private loopback mTLS fixture. Run after other GPU tests. No production edits.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def invoke(words, timeout=180):
    start = time.perf_counter()
    result = subprocess.run(list(map(str, words)), capture_output=True, text=True, timeout=timeout)
    elapsed = (time.perf_counter() - start) * 1000
    if result.returncode:
        raise RuntimeError((words, result.returncode, result.stdout, result.stderr))
    return [json.loads(line) for line in result.stdout.splitlines()], elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--apache-root', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    sys.path.insert(0, str(source / 'tools'))
    from coordinator_local import Environment, HOST
    keyhunt, worker = build / 'keyhunt', build / 'keyhunt-worker'
    supervisor = source / 'tools/coordinator_worker.py'
    report = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), revision=args.revision,
                  sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (keyhunt, worker, supervisor)},
                  visibility={k: os.environ.get(k) for k in
                              ('HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES')},
                  inventory=invoke([keyhunt, 'devices', '--backend', 'hip'])[0][0],
                  warm=[], supervised=[], pause_probes=[])

    def save():
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')

    for repeat in range(3):
        for mode in (('xpoint', 'bsgs') if repeat % 2 == 0 else ('bsgs', 'xpoint')):
            executable = build / ('hip_xpoint_benchmark' if mode == 'xpoint' else 'hip_bsgs_search_benchmark')
            words = ['0', '1048576'] if mode == 'xpoint' else ['0', '65537', '32768']
            rows, wall = invoke([executable, *words])
            report['warm'].append(dict(mode=mode, repeat=repeat, data=rows[0], process_wall_ms=wall))
        save()
        print(f'warm benchmark pair {repeat + 1}/3 passed', flush=True)

    with tempfile.TemporaryDirectory(prefix='kh-c15-audit-') as temporary:
        root = Path(temporary)
        env = Environment(root / 'server', build / 'keyhunt-coordinator', args.apache_root)
        children = []
        try:
            env.initialize()
            env.start()
            alice = env.admin('bootstrap', name='synthetic audit', certificate=(env.directory / 'alice.pem').read_text())
            project = env.admin('project-create', name='C15 audit', owner=alice['client'])['project']

            def api(method, path, body=None):
                with env.client('alice') as client:
                    client.request(method, path, None if body is None else json.dumps(body), {'Content-Type': 'application/json'})
                    response = client.getresponse()
                    payload = response.read()
                    assert response.status == 200, (response.status, payload)
                    return json.loads(payload)['value']

            def native(state, action, *extra):
                return invoke([worker, action, '--state-dir', state, *extra])[0][0]

            def configure(name, width, blocks=2):
                begin = 1 << 200
                body = dict(mode='xpoint', begin=f'0x{begin:064x}',
                            end_exclusive=f'0x{begin + blocks * width:064x}', block_width=f'0x{width:064x}',
                            configuration=(b'khsearch\x01\x01' + bytes(40)).hex(),
                            targets='79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798')
                job = api('POST', f'/api/v1/projects/{project}/jobs', body)['job']
                state = root / name
                config = dict(endpoint='https://' + env.authority, ca=str(env.directory / 'server-ca.pem'),
                              certificate=str(env.directory / 'alice.pem'), key=str(env.directory / 'alice.key'),
                              resolve=f'{HOST}:{env.port}:127.0.0.1',
                              jobs=[dict(project=project, job=job, devices=['0'], spares=blocks - 1, policy='sequential')])
                path = root / (name + '.json')
                path.write_text(json.dumps(config))
                path.chmod(0o600)
                native(state, 'configure', '--config', path)
                native(state, 'sync')
                assert len(native(state, 'status')['queues']) == blocks
                return state, job

            def command(state):
                return [sys.executable, str(supervisor), '--state-dir', str(state),
                        '--worker', str(worker), '--keyhunt', str(keyhunt), '--once', '--stall-seconds', '60']

            # Three independent two-block jobs per size. Setup and first HTTPS
            # sync excluded; fresh self-test, process startup and cleanup included.
            for repeat in range(3):
                sizes = (1 << 20, 1 << 32) if repeat % 2 == 0 else (1 << 32, 1 << 20)
                for width in sizes:
                    state, job = configure(f'cost-{repeat}-{width}', width + repeat)
                    start = time.perf_counter()
                    result = subprocess.run(command(state), capture_output=True, text=True, timeout=180)
                    wall = (time.perf_counter() - start) * 1000
                    assert result.returncode == 0, (result.stderr, (state / 'execution.log').read_text())
                    status = native(state, 'status')
                    assert len(status['queues']) == 2
                    assert all(q['activity'] == 'local-complete-awaiting-sync' for q in status['queues'])
                    assert native(state, 'next', '--device', '0') is None
                    assert not native(state, 'scheduled-sync')['sent']
                    summary = json.loads((state / 'execution.log').read_text().splitlines()[-1])
                    assert summary['complete'] and int(summary['computed_scalars'], 16) == width + repeat
                    server_before = api('GET', f'/api/v1/projects/{project}/jobs/{job}/status')
                    assert int(server_before['finished'], 16) == 0
                    native(state, 'sync')
                    server_after = api('GET', f'/api/v1/projects/{project}/jobs/{job}/status')
                    assert int(server_after['finished'], 16) == 2
                    invoke([keyhunt, 'state', 'check', '--state-dir', state])
                    report['supervised'].append(dict(repeat=repeat, base_block_width=width, block_width=width + repeat,
                        blocks=2, process_wall_ms=wall, scalar_range_per_second=2 * (width + repeat) * 1000 / wall,
                        status_before_upload=status, last_block_summary=summary))
                    save()
                    print(f'supervised two-block run {repeat + 1}/3 width={width} passed', flush=True)

            # Server-requested pauses should not become hardware failures. Use
            # explicit manual syncs to observe the current control immediately.
            state, job = configure('server-pauses', 1 << 61, blocks=1)
            log = open(root / 'server-pauses.log', 'w')
            process = subprocess.Popen([v for v in command(state) if v != '--once'], stdout=log, stderr=subprocess.STDOUT)
            children.append(process)
            cycles = []
            for cycle in range(3):
                deadline = time.monotonic() + 45
                while True:
                    if process.poll() is not None:
                        break
                    try:
                        current = invoke([keyhunt, 'checkpoint', 'status', '--state-dir', state], timeout=10)[0][0]
                        if current['state'] == 'running':
                            break
                    except (RuntimeError, subprocess.TimeoutExpired):
                        pass
                    assert time.monotonic() < deadline, 'server pause owner did not become ready'
                    time.sleep(.05)
                if process.poll() is not None:
                    break
                api('POST', f'/api/v1/projects/{project}/jobs/{job}/pause', {'paused': True})
                native(state, 'sync')
                deadline = time.monotonic() + 20
                while True:
                    saved = json.loads((state / 'supervisor.json').read_text())
                    if saved.get('failures', {}).get('0', 0) >= cycle + 1 or process.poll() is not None:
                        break
                    assert time.monotonic() < deadline, 'supervisor did not observe paused child exit'
                    time.sleep(.1)
                cycles.append(dict(cycle=cycle, supervisor=saved,
                                   execution_log=(state / 'execution.log').read_text()))
                api('POST', f'/api/v1/projects/{project}/jobs/{job}/pause', {'paused': False})
                native(state, 'sync')
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=40)
            log.close()
            report['server_pause_cycles'] = dict(cycles=cycles,
                supervisor_final=json.loads((state / 'supervisor.json').read_text()),
                locally_executable_grant_after_unpause=native(state, 'next', '--device', '0'),
                log=(root / 'server-pauses.log').read_text())
            invoke([keyhunt, 'state', 'check', '--state-dir', state])
            save()
            print('server pause cycles completed; journal integrity passed', flush=True)

            # Actual 60-second watchdog, real HIP owner. Observe both supervisor
            # signals and direct C14 socket pause. No clock or implementation patch.
            for probe in ('supervisor_signal', 'checkpoint_socket'):
                state, _job = configure('pause-' + probe, (1 << 60) + len(report['pause_probes']), blocks=1)
                log = open(root / ('supervisor-' + probe + '.log'), 'w')
                process = subprocess.Popen(command(state), stdout=log, stderr=subprocess.STDOUT)
                children.append(process)
                deadline = time.monotonic() + 45
                while True:
                    if process.poll() is not None:
                        raise RuntimeError('supervisor exited before pause: ' + (root / ('supervisor-' + probe + '.log')).read_text())
                    if (state / 'control.sock').exists():
                        try:
                            current = invoke([keyhunt, 'checkpoint', 'status', '--state-dir', state], timeout=10)[0][0]
                            if current['state'] == 'running':
                                break
                        except (RuntimeError, subprocess.TimeoutExpired):
                            pass
                    assert time.monotonic() < deadline, 'GPU owner did not become ready'
                    time.sleep(.05)
                if probe == 'supervisor_signal':
                    process.send_signal(signal.SIGUSR1)
                else:
                    invoke([keyhunt, 'checkpoint', 'pause', '--state-dir', state])
                deadline = time.monotonic() + 20
                while True:
                    paused = invoke([keyhunt, 'checkpoint', 'status', '--state-dir', state])[0][0]
                    if paused['durably_paused']:
                        break
                    assert time.monotonic() < deadline, 'owner did not durably pause'
                    time.sleep(.05)
                print(f'{probe}: durably paused; observing watchdog for 62 seconds', flush=True)
                pause_start = time.monotonic()
                while time.monotonic() - pause_start < 62 and process.poll() is None:
                    time.sleep(.2)
                resumed = False
                if probe == 'supervisor_signal' and process.poll() is None:
                    process.send_signal(signal.SIGUSR2)
                    resumed = True
                    time.sleep(2)
                observed_exit = process.poll()
                before_stop = json.loads((state / 'supervisor.json').read_text())
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=40)
                log.close()
                final = json.loads((state / 'supervisor.json').read_text())
                execution = (state / 'execution.log').read_text()
                invoke([keyhunt, 'state', 'check', '--state-dir', state])
                report['pause_probes'].append(dict(probe=probe, paused=paused, resumed=resumed,
                    observed_exit_before_audit_cleanup=observed_exit, supervisor_before_cleanup=before_stop,
                    supervisor_final=final, execution_records=[json.loads(line) for line in execution.splitlines()
                                                             if line.startswith('{')],
                    supervisor_log=(root / ('supervisor-' + probe + '.log')).read_text()))
                save()
                print(f'{probe}: failures={final["failures"]}; journal integrity passed', flush=True)
            env.admin('check')
        finally:
            for process in children:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            env.stop()
    report['completed'] = True
    save()
    print(f'audit evidence saved to {args.report}', flush=True)


if __name__ == '__main__':
    main()
