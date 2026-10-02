#!/usr/bin/env python3
"""Read-only C20/C21 evidence audit; optional modeled terminal-owner hang probe.

Use a frozen --source checkout. No production state or GPU is touched by this
tool. --fleet can additionally check newly measured validate_fleet.py reports.
The terminal probe runs a synthetic child, never injects a GPU driver fault.
"""
import argparse
from collections import Counter, defaultdict
from fractions import Fraction
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[name] = loaded
    spec.loader.exec_module(loaded)
    return loaded


def distribution(values):
    require(bool(values), 'empty distribution')
    return dict(count=len(values), median=statistics.median(values), min=min(values), max=max(values))


def fleet(data, oracle):
    require(data['passed'], 'fleet did not finish validation')
    gx = oracle.encode(oracle.multiply(1))
    neg = oracle.encode(oracle.multiply(oracle.N - 1))
    identities = {str(d['ordinal']): d['uuid'] for d in data['inventory']['devices']}
    groups = defaultdict(list)
    blocks = 0
    for run in data['runs']:
        n, mode = run['device_count'], run['mode']
        require(run['validated'] and run['measured'] == (run['repetition'] > 0), 'invalid repetition')
        require(run['targets'] == (gx[2:66] if mode == 'xpoint' else gx + neg), 'unexpected target fixture')
        require(len(run['grants']) == 2 * n, 'missing block completion')
        grants = sorted(run['grants'], key=lambda row: int(row['grant']['begin'], 16))
        width = int(grants[0]['grant']['end_exclusive'], 16) - 1
        cursor = 1
        queues = defaultdict(list)
        for i, row in enumerate(grants):
            g = row['grant']
            begin, end = int(g['begin'], 16), int(g['end_exclusive'], 16)
            require(row['complete'] and begin == cursor and end > begin, 'overlap, gap or incomplete grant')
            require(int(g['block'], 16) == i and end - begin == width - (7 if i == 2*n-1 else 0), 'wrong grid/tail')
            require(int(row['computed_scalars'], 16) == end-begin, 'wrong credited coverage')
            m, targets = (1, 1) if mode == 'xpoint' else (257, 2)
            steps = ((end-begin + m-1)//m) * targets
            require(row['m'] == m and row['target_count'] == targets and row['mode'] == mode, 'wrong algorithm inputs')
            require(int(row['device_steps'], 16) == steps, 'wrong actual device work')
            require(row['uuid'] == identities[row['queue']], 'device identity mismatch')
            require(row['executor_setups'] == 1 and row['wall_ns'] > 0 and row['kernel_ms'] > 0, 'setup/timing invalid')
            queues[row['queue']].append(row)
            cursor = end
        require(set(queues) == set(map(str, range(n))), 'selected device did no work')
        for rows in queues.values():
            require(sum(row['cold'] for row in rows) == 1, 'executor recreated or never initialized')
        require(cursor-1 == int(run['scalar_count']), 'fleet coverage mismatch')
        require(run['wall_ns'] > 0, 'invalid fleet elapsed time')
        groups[mode, n].append(run)
        blocks += len(grants)
    metrics = []
    for (mode, n), runs in sorted(groups.items()):
        require(Counter(r['repetition'] for r in runs) == Counter(range(len(runs))), 'missing/duplicate repetitions')
        measured = [r for r in runs if r['measured']]
        rates = [int(r['scalar_count']) * 1e9 / r['wall_ns'] for r in measured]
        one = [int(r['scalar_count']) * 1e9 / r['wall_ns'] for r in groups[mode, 1] if r['measured']]
        # Sum the sequential grants for each owner, then take the slowest owner.
        # Subtracting that from process time estimates time outside grant bodies;
        # it does not identify one specific startup/IPC/durability bottleneck.
        residual = []
        for r in measured:
            by_queue = defaultdict(int)
            for g in r['grants']:
                by_queue[g['queue']] += g['wall_ns']
            residual.append((r['wall_ns'] - max(by_queue.values()))/1e9)
        metrics.append(dict(mode=mode, devices=n, rates=distribution(rates),
                            seconds=distribution([r['wall_ns']/1e9 for r in measured]),
                            relative_to_one=statistics.median(rates)/statistics.median(one),
                            outside_slowest_owner_grants_seconds=distribution(residual)))
    return dict(runs=len(data['runs']), blocks=blocks, metrics=metrics)


def calibration(data, saved):
    for mode, recommendation in saved.items():
        rows = [g for r in data['runs'] if r['measured'] and r['device_count'] == 1 and r['mode'] == mode
                for g in r['grants'] if not g['cold'] and g['queue'] == recommendation['reference_queue']]
        require(len(rows) == recommendation['samples'] >= 5, 'calibration sample count')
        rate = statistics.median(Fraction(int(g['computed_scalars'],16)*10**9,g['wall_ns']) for g in rows)
        steps = statistics.median(Fraction(int(g['device_steps'],16)*10**9,g['wall_ns']) for g in rows)
        for key, value in [('effective_scalars_per_second', rate), ('device_steps_per_second', steps)]:
            require(recommendation[key] == dict(numerator=str(value.numerator),denominator=str(value.denominator)), 'rate mismatch')
        alignment = 1 if mode == 'xpoint' else rows[0]['m']
        width = math.ceil(rate * 43200 / alignment) * alignment
        work = max(alignment, math.floor(rate * 180 / alignment) * alignment)
        require(int(recommendation['block_width'],16) == width, 'calibrated block mismatch')
        require(int(recommendation['initial_work_unit_width'],16) == work, 'calibrated work mismatch')


def evidence(source, extra):
    base = source/'docs/baselines'
    oracle = module(source/'tests/oracle/model.py', 'c21_oracle')
    out = dict(manifests={}, fleets={}, publication_files=0, quickstarts={}, operations={})
    for name in ('C20_VALIDATION', 'C20_CUDA_VALIDATION', 'C21_VALIDATION'):
        doc = json.loads((base/(name+'.json')).read_text())
        checked, drift = 0, []
        for path, digest in doc.get('source_sha256', {}).items():
            old = subprocess.check_output(['git','-C',str(source),'show',doc['source_commit']+':'+path])
            if sha(old) != digest:
                # HIP recorded an uncommitted validation-harness update, later
                # committed with its artifacts. Preserve that provenance detail.
                require(name == 'C20_VALIDATION' and path == 'tools/validate_fleet.py' and
                        sha((source/path).read_bytes()) == digest, 'unresolved source fingerprint: '+path)
                drift.append(path)
            checked += 1
        for path, digest in doc.get('publication_sha256', {}).items():
            require(sha((source/path).read_bytes()) == digest, 'publication fingerprint: '+path)
            out['publication_files'] += 1
        for path, expected in doc['artifacts'].items():
            digest = expected['sha256'] if isinstance(expected,dict) else expected
            require(sha((base/path).read_bytes()) == digest, 'artifact fingerprint: '+path)
        archive_name = {'C20_VALIDATION':'C20_TEST_LOGS','C20_CUDA_VALIDATION':'C20_CUDA_TEST_LOGS','C21_VALIDATION':'C21_LOGS'}[name]
        members = doc.get('test_logs',doc.get('log_members',{}))
        with tarfile.open(base/(archive_name+'.tar.gz')) as archive:
            for member, digest in members.items():
                require(sha(archive.extractfile(member).read()) == digest, 'log fingerprint: '+member)
        out['manifests'][name] = dict(source_files=checked, log_members=len(members), recorded_worktree_differences=drift)
    for prefix in ('C20', 'C20_CUDA'):
        data = json.loads((base/(prefix+'_FLEET.json')).read_text())
        found = fleet(data,oracle)
        published = json.loads((base/(prefix+'_VALIDATION.json')).read_text())['fleet']
        require(found['runs'] == published['validated_runs'] and found['blocks'] == published['block_completions'], 'fleet totals')
        for metric in found['metrics']:
            previous = next(m for m in published['metrics'] if m['mode'] == metric['mode'] and m['devices'] == metric['devices'])
            require(math.isclose(previous['median_effective_scalars_per_second'],metric['rates']['median'],rel_tol=1e-12), 'published rate mismatch')
            require(math.isclose(previous['relative_to_one_device'],metric['relative_to_one'],rel_tol=1e-12), 'published scaling mismatch')
        calibration(data,json.loads((base/(prefix+'_CALIBRATION.json')).read_text()))
        out['fleets'][prefix] = found
    examples = json.loads((base/'C21_EXAMPLES.json').read_text())
    validator = module(source/'tests/integration/gpu_examples.py','c21_examples')
    for backend, run in examples['runs'].items():
        require(run['passed'] and run['exit_code'] == 0, 'quickstart failure')
        validator.validate(run['artifacts'],backend != 'cpu',run['device'])
        out['quickstarts'][backend] = dict(artifacts=len(run['artifacts']), hardware=run['hardware_executed'])
    for backend, run in json.loads((base/'C21_OPERATIONS.json').read_text())['runs'].items():
        require(run['passed'] and run['launcher_exit_code'] == 0 and all(c['exit_code']==0 for c in run['commands']), 'operations failure')
        before, _, local, ack, check = [json.loads(c['stdout']) if c['stdout'].strip() else None for c in run['commands']]
        require(local == run['local_status'] and ack == run['acknowledged_status'], 'operation output mismatch')
        require(local['outbox_bytes'] > 0 and local['last_ack_server_time'] == before['last_ack_server_time'], 'hidden sync')
        require(ack['sent'] and ack['outbox_bytes'] == 0 and check['integrity'] == 'ok', 'ack/integrity failure')
        out['operations'][backend] = dict(commands=len(run['commands']), passed=True)
    for path in extra:
        out['fleets'][str(path)] = fleet(json.loads(path.read_text()),oracle)
    out['passed'] = True
    return out


def terminal_probe(source):
    supervisor = module(source/'tools/coordinator_worker.py','c21_supervisor')
    device = supervisor.Device('0','0',child=object(),state='running',last_progress=1)
    device.event({'type':'control','state':'stopped'},2)
    report = dict(modeled_fault='owner hangs after publishing stopped, before process exit',
                  deterministic_stalled=device.stalled(1000002,60))
    with tempfile.TemporaryDirectory(prefix='kh-c21-terminal-') as temp:
        root=Path(temp); fake=root/'worker'; state=root/'state'; state.mkdir(mode=0o700)
        fake.write_text('''#!/usr/bin/env python3
import sys,json,time
act=sys.argv[1]
if act=='configuration':print(json.dumps({'jobs':[{'devices':['0']}]}))
elif act=='status':print(json.dumps({'sync_due_in':7200,'queues':[]}))
elif act=='scheduled-sync':print(json.dumps({'sent':False}))
elif act=='run-device':
 print(json.dumps({'type':'ready','uuid':'audit-fake','self_test':{'passed':True}}),flush=True)
 print(json.dumps({'type':'control','state':'stopped'}),flush=True)
 print(json.dumps({'type':'exit','reason':'drained','completed':0,'executor_setups':0}),flush=True)
 time.sleep(3600)
'''); fake.chmod(0o700)
        command=[sys.executable,str(source/'tools/coordinator_worker.py'),'--state-dir',str(state),
                 '--worker',str(fake),'--keyhunt',str(fake),'--once','--stall-seconds','60']
        process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            deadline=time.monotonic()+12
            while time.monotonic()<deadline:
                if (state/'supervisor.json').exists():
                    status=json.loads((state/'supervisor.json').read_text())
                    if status['devices']['0']['state']=='stopped':break
                time.sleep(.1)
            else:raise ValueError('missing terminal event')
            start=time.monotonic()
            while time.monotonic()-start<65:time.sleep(.5)
            status=json.loads((state/'supervisor.json').read_text())
            report.update(observed_seconds=time.monotonic()-start,status=status,supervisor_alive=process.poll() is None)
            report['reproduced']=report['supervisor_alive'] and not status['failures'] and status['devices']['0']['pid'] is not None
            process.terminate(); output,_=process.communicate(timeout=40)
            report.update(manual_stop_exit_code=process.returncode,output=output.decode())
        finally:
            if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait()
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--fleet',type=Path,action='append',default=[])
    parser.add_argument('--terminal-probe',action='store_true')
    args=parser.parse_args(); source=args.source.resolve()
    report=terminal_probe(source) if args.terminal_probe else evidence(source,args.fleet)
    report['source_commit']=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    report['audit_tool_sha256']=sha(Path(__file__).read_bytes())
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print('Audit evidence written to',args.output)


if __name__=='__main__':
    main()
