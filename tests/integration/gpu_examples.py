#!/usr/bin/env python3
"""Execute the marked GPU quickstart blocks, including CPU-only preparation in CI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DOCUMENT = ROOT / 'docs/GPU_QUICKSTART.md'
BLOCKS = ('prepare', 'volatile', 'create', 'durable')


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    # These checks must still run if the harness is invoked with python -O.
    if not condition:
        raise ValueError(message)


def validate(artifacts, hardware, device):
    def one(name):
        return artifacts[name + '.json']

    require(one('table-inspect')['m'] == 257, 'wrong baby-table size')
    require(one('preflight')['integrity'] == 'ok', 'preflight audit failed')
    for mode, width in [('xpoint', 256), ('bsgs', 65536)]:
        job = one(mode + '-job')
        assignments = one(mode + '-grant')['assignments']
        require(job['project'] == one('project')['project'], 'wrong project')
        require(len(assignments) == 1 and assignments[0]['grant'], 'missing single grant')
        grant = assignments[0]
        require(grant['job'] == job['job'] and grant['project'] == job['project'] and
                int(grant['block'], 16) == 0 and int(grant['begin'], 16) == 1 and
                int(grant['end_exclusive'], 16) == width + 1, 'wrong assignment bounds')
        if not hardware:
            continue
        summary = artifacts[mode + '.ndjson'][-1]
        require(summary['type'] == 'summary' and summary['complete'] and
                not summary['durable_coverage'], 'wrong volatile completion')
        # Verify the complete receipt union as well as the known public fixture.
        cursor, matches = 1, []
        for record in artifacts[mode + '.ndjson'][1:-1]:
            if record.get('overflow'):
                continue
            if record['type'] == 'batch':
                matches.extend(record['matches'])
            # BSGS accepts coverage only at the all-target tile receipt. Its
            # preceding batch receipts can describe the same scalar interval.
            coverage_type = 'batch' if mode == 'xpoint' else 'tile'
            if record['type'] == coverage_type:
                begin = int(record['begin'], 16)
                end = int(record['end_exclusive'], 16)
                require(begin == cursor and end > begin, 'noncontiguous volatile coverage')
                cursor = end
        require(cursor == 1 + width, 'wrong volatile endpoint')
        require(len(matches) == 1 and int(matches[0]['scalar'], 16) == 1,
                'wrong volatile match set')
        durable = artifacts[mode + '-durable.ndjson'][-1]
        require(durable['complete'] and durable['durability'] == 'local', 'incomplete durable run')
        require(int(durable['computed_scalars'], 16) == width, 'wrong durable coverage')
        retry = artifacts[mode + '-retry.ndjson'][-1]
        require(retry['complete'] and retry['batches'] == 0 and
                int(retry['resumed_scalars'], 16) == width and
                int(retry['computed_scalars'], 16) == 0, 'completed grant recomputed work')
        results = one(mode + '-results')['results']
        require(len(results) == 1 and int(results[0]['scalar'], 16) == 1,
                'wrong durable match set')
        block = one(mode + '-block')
        require(block['state'] == 'finished' and not block['remaining'], 'block not finished')
        require([(int(row['begin'], 16), int(row['end_exclusive'], 16))
                 for row in block['covered']] == [(1, 1 + width)], 'wrong durable interval union')
    if hardware:
        require(one('check')['integrity'] == 'ok', 'final journal audit failed')
        require(one('table-validate')['m'] == 257 and
                one('table-validate')['checksum'] == one('table-inspect')['checksum'],
                'GPU table validation changed the table')
        require(any(item['ordinal'] == device for item in one('devices')['devices']),
                'selected device absent')
        smoke = one('smoke')
        require(smoke['diagnostic_only'] and not smoke['search_coverage'] and
                smoke['device'] == device and smoke['device_steps'] == 257,
                'diagnostic incorrectly claims search coverage')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--backend', choices=('cpu', 'hip', 'cuda'), required=True)
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    require(args.device >= 0, 'device must be nonnegative')
    hardware = args.backend != 'cpu'
    # Markers make the runnable contract explicit; prose/benchmark examples are
    # never guessed or executed. Missing, duplicate or reordered blocks fail CI.
    blocks = re.findall(r'<!-- example: (\w+) -->\n```bash\n(.*?)\n```',
                        DOCUMENT.read_text(), re.S)
    require(tuple(name for name, _ in blocks) == BLOCKS, 'quickstart block inventory changed')
    selected = [(name, code) for name, code in blocks
                if hardware or name in ('prepare', 'create')]
    script = '\n\n'.join(code for _, code in selected) + '\n'
    report = dict(backend=args.backend, hardware_executed=hardware, device=args.device,
                  binary_sha256=sha256(binary), document_sha256=sha256(DOCUMENT),
                  harness_sha256=sha256(Path(__file__)),
                  script_sha256=hashlib.sha256(script.encode()).hexdigest(),
                  blocks=[name for name, _ in selected], passed=False, artifacts={})
    # The document owns one child of this private directory. Removing the parent
    # on exit cannot touch an operator's existing state, grants or result files.
    with tempfile.TemporaryDirectory(prefix='keyhunt-docs-', dir='/var/tmp') as temporary:
        env = dict(os.environ, KEYHUNT_BIN=str(binary), GPU_BACKEND=args.backend,
                   GPU_DEVICE=str(args.device), EXAMPLE_PARENT=temporary)
        started = time.monotonic()
        process = subprocess.Popen(['bash', '-c', script], cwd=ROOT, env=env,
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            # Include native children in timeout cleanup, not just their shell.
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            report['error'] = 'documented commands exceeded 180 seconds'
        report.update(exit_code=process.returncode, stdout=stdout, stderr=stderr,
                      wall_seconds=time.monotonic() - started)
        try:
            require(process.returncode == 0, 'documented command failed')
            directories = list(Path(temporary).glob('keyhunt-example.*'))
            require(len(directories) == 1, 'missing example output directory')
            for path in sorted(directories[0].iterdir()):
                if path.suffix == '.json':
                    report['artifacts'][path.name] = json.loads(path.read_text())
                elif path.suffix == '.ndjson':
                    report['artifacts'][path.name] = [json.loads(line) for line in path.read_text().splitlines()]
            validate(report['artifacts'], hardware, args.device)
            report['passed'] = True
        except (ValueError, KeyError, TypeError, IndexError) as error:
            report['error'] = str(error)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS' if report['passed'] else 'FAIL') + f' {args.backend} documented quickstart')
    if not report['passed']:
        print(report.get('error', 'failed'))
        print(report['stderr'])
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
