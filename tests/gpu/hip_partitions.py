#!/usr/bin/env python3
"""Exercise every currently visible HIP device without changing partition modes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    report = {'recorded_utc': datetime.now(timezone.utc).isoformat(), 'commands': [],
              'launches': [], 'failures': [], 'partition_changes': False}

    def run(arguments, env=None):
        command = [str(binary), *arguments]
        result = subprocess.run(command, text=True, capture_output=True, timeout=30, env=env)
        report['commands'].append({'argv': command, 'exit_code': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(f'{arguments[0]} failed: {result.stderr}')
        return json.loads(result.stdout)

    def launch(device, start, count, env=None):
        result = run(['gpu-smoke', '--backend', 'hip', '--device', str(device['ordinal']),
                      '--steps', str(count), '--start', hex(start)], env)
        if not (result['uuid'] == device['uuid'] and result['device'] == device['ordinal']
                and result['device_steps'] == count and result['launch_count'] == 1
                and result['diagnostic_only'] and result['search_coverage'] is False
                and int(result['begin'], 16) == start and int(result['end_exclusive'], 16) == start + count):
            raise RuntimeError('launch identity or exact index bounds differ from the request')
        report['launches'].append(result)

    try:
        report['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        report['visibility'] = {key: os.environ.get(key) for key in
                                ['HIP_VISIBLE_DEVICES', 'ROCR_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES']}
        inventory = run(['devices', '--backend', 'hip'])
        report['inventory'] = inventory
        devices = inventory['devices']
        if not devices:
            raise RuntimeError('HIP hardware required: no devices visible')
        for ordinal, device in enumerate(devices):
            if device['ordinal'] != ordinal:
                raise RuntimeError('noncontiguous runtime ordinals')
            # Do not infer device counts, CU counts or memory limits from a mode
            # label. This also accepts secondary partitions with absent sysfs.
            launch(device, 1, 1)
            launch(device, 0x100000000ffffffffffffffff, 257)
        if len(devices) > 1 and not any(os.environ.get(key) for key in
                                       ['HIP_VISIBLE_DEVICES', 'CUDA_VISIBLE_DEVICES']):
            # Preserve any ROCR filter. With no existing HIP/CUDA ordinal filter,
            # these ordinals refer to that same restricted ROCr inventory.
            visible = f'{len(devices) - 1},0'
            environment = dict(os.environ, HIP_VISIBLE_DEVICES=visible)
            reordered = run(['devices', '--backend', 'hip'], environment)['devices']
            expected = [devices[-1]['uuid'], devices[0]['uuid']]
            if [d['uuid'] for d in reordered] != expected:
                raise RuntimeError('HIP visibility remapping changed device identity')
            report['remapping'] = {'HIP_VISIBLE_DEVICES': visible, 'devices': reordered}
            for device in reordered:
                launch(device, 0xffffffffffffffff, 257, environment)
        else:
            report['remapping'] = {'skipped': 'one device or an existing HIP/CUDA filter; preserve caller restrictions'}
        # Rediscover after the launches: free memory may change, but identity and
        # reported partition modes must not change underneath this process run.
        after = run(['devices', '--backend', 'hip'])['devices']
        identity = lambda ds: [(d['uuid'], d['compute_partition'], d['memory_partition']) for d in ds]
        if identity(after) != identity(devices):
            raise RuntimeError('device topology changed during validation; restart and rediscover')
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
        report['failures'].append(str(error))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f"HIP partition checks: {len(report.get('inventory', {}).get('devices', []))} devices, "
          f"{len(report['launches'])} launches, {len(report['failures'])} failures; {args.report}")
    return bool(report['failures'])


if __name__ == '__main__':
    raise SystemExit(main())
