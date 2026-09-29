#!/usr/bin/env python3
"""Capture C07 identity/build/launch evidence; these are not search benchmarks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
ORDER = int('fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141', 16)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build/hip-release')
    p.add_argument('--device', type=int, default=0)
    p.add_argument('--report', type=Path, required=True)
    args = p.parse_args()
    binary = args.build_dir.resolve() / 'keyhunt'
    report = {'recorded_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'diagnostic_only': True, 'failures': []}

    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, capture_output=True,
                                text=True, timeout=60)
        report['commands'].append({'argv': [str(x) for x in command],
                                   'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(f'command failed: {command}')
        return result.stdout

    try:
        report['source_commit'] = run(['git', 'rev-parse', 'HEAD']).strip()
        report['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        report['visibility'] = {k: os.environ.get(k) for k in
                                ['HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES']}
        report['cmake_version'] = run(['cmake', '--version'])
        compile_commands = json.loads((args.build_dir / 'compile_commands.json').read_text())
        production = [x for x in compile_commands if x['file'].endswith('.hip')
                      and 'keyhunt_backend.dir' in x['command']]
        if len(production) != 2:
            raise RuntimeError('expected discovery and executor HIP translation units')
        report['hip_compile_commands'] = production
        forbidden = {'-m64', '-mssse3', '-march=native', '-mtune=native', '-Ofast',
                     '-ffast-math', '-flto', '-ftree-vectorize'}
        for entry in production:
            if forbidden.intersection(shlex.split(entry['command'])):
                raise RuntimeError('CPU/fast-math flags leaked into HIP compilation')
        compiler = shlex.split(production[0]['command'])[0]
        report['hip_compiler_version'] = run([compiler, '--version'])
        inventory = json.loads(run([binary, 'devices', '--backend', 'hip']))
        report['inventory'] = inventory
        if not 0 <= args.device < len(inventory['devices']):
            raise RuntimeError('requested device is not visible')
        # Separate processes deliberately measure launch diagnostics, not warmed
        # steady-state throughput. Host verification covers every emitted index.
        cases = [(1, 1), (0xffffffff, 255), (0xffffffffffffffff, 256),
                 (0x100000000ffffffffffffffff, 257), (1 << 192, 4097),
                 (ORDER - 3, 3), (1 << 128, 1048576)]
        report['launches'] = []
        for begin, count in cases:
            result = json.loads(run([binary, 'gpu-smoke', '--backend', 'hip',
                                     '--device', args.device, '--start', hex(begin), '--steps', count]))
            if not (result['diagnostic_only'] and result['search_coverage'] is False
                    and result['device_steps'] == count and result['launch_count'] == 1
                    and int(result['begin'], 16) == begin
                    and int(result['end_exclusive'], 16) == begin + count
                    and result['uuid'] == inventory['devices'][args.device]['uuid']):
                raise RuntimeError('diagnostic count, bounds, or device identity mismatch')
            report['launches'].append(result)
        report['note'] = ('Separate short processes; timings exclude allocation/startup and are not '
                          'search throughput or evidence of transfer overlap. No hardware settings changed.')
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report['failures'].append(str(error))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HIP baseline: {len(report.get('launches', []))} launches, {len(report['failures'])} failures; {args.report}")
    return bool(report['failures'])


if __name__ == '__main__':
    raise SystemExit(main())
