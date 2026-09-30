#!/usr/bin/env python3
"""Check discovery errors and JSON identity without starting a search."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True)
p.add_argument('--hardware','--hip',dest='hardware', action='store_true')
p.add_argument("--backend",choices=("hip","cuda"),default="hip")
a = p.parse_args()


def run(args, env=None):
    return subprocess.run([a.binary, *args], capture_output=True, text=True,
                          timeout=30, env=env)


for args in [['devices'], ['devices', '--backend', 'invalid'],
             ['devices', '--backend', a.backend, '--unknown']]:
    r = run(args)
    assert r.returncode == 2 and 'usage:' in r.stderr and not r.stdout, r
r = run(['devices', '--backend', a.backend])
if not a.hardware:
    assert r.returncode == 2 and 'not built' in r.stderr and not r.stdout, r
else:
    assert r.returncode == 0, r
    inventory = json.loads(r.stdout)
    assert inventory['backend'] == a.backend
    seen = set()
    for i, d in enumerate(inventory['devices']):
        assert d['ordinal'] == i and len(d['uuid']) == 32 and d['uuid'] not in seen
        seen.add(d['uuid'])
        assert d['name'] and d['architecture'] and d['pci_bus_id']
        assert 0 <= d['free_memory_bytes'] <= d['total_memory_bytes']
        assert d['compute_units'] > 0 and d['warp_size'] > 0
        assert d['physical_id'] or 'physical package identity unavailable' in d['warnings']
        if a.backend == 'cuda':
            assert d['pci_bus_id'] == d['pci_bus_id'].lower()
            numa = Path('/sys/bus/pci/devices') / d['pci_bus_id'] / 'numa_node'
            if numa.exists():
                assert d['numa_node'] == int(numa.read_text()), d
    # No visible devices is valid discovery, but never a fallback CPU search.
    env = dict(os.environ, CUDA_VISIBLE_DEVICES='-1', HIP_VISIBLE_DEVICES='-1', ROCR_VISIBLE_DEVICES='-1')
    r = run(['devices', '--backend', a.backend], env)
    assert r.returncode == 0 and json.loads(r.stdout)['devices'] == [], r
for args in [[], ['--backend', 'invalid'], ['--backend', a.backend, '--steps', '0'],
             ['--backend', a.backend, '--steps', '1048577'], ['--backend', a.backend, '--steps', '-1'],
             ['--backend', a.backend, '--device', '2147483648'],
             ['--backend', a.backend, '--steps', '2junk'], ['--backend', a.backend, '--unknown', '1'],
             ['--backend', a.backend, '--device', '0', '--device', '0']]:
    r = run(['gpu-smoke', *args])
    assert r.returncode == 2 and not r.stdout, r
r = run(['gpu-smoke', '--backend', a.backend, '--steps', '257'])
if a.hardware:
    assert r.returncode == 0, r
    result = json.loads(r.stdout)
    assert result['diagnostic_only'] and result['search_coverage'] is False
    assert result['device_steps'] == 257 and result['launch_count'] == 1
    assert int(result['end_exclusive'], 16) - int(result['begin'], 16) == 257
    for args in [['--device', '2147483647'], ['--start', '0'], ['--start', 'xyz'],
                 ['--start', 'ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff']]:
        r = run(['gpu-smoke', '--backend', a.backend, *args])
        assert r.returncode == 2 and not r.stdout, r
    r = run(['gpu-smoke', '--backend', a.backend], dict(os.environ, CUDA_VISIBLE_DEVICES='-1', HIP_VISIBLE_DEVICES='-1', ROCR_VISIBLE_DEVICES='-1'))
    assert r.returncode == 2 and 'not visible' in r.stderr and not r.stdout, r
else:
    assert r.returncode == 2 and 'not built' in r.stderr and not r.stdout, r
print('Backend discovery and diagnostic CLI checks passed')

# A binary must never silently execute a different runtime from the requested one.
for unavailable in (("hip", "cuda") if not a.hardware else
                    ("hip" if a.backend == "cuda" else "cuda",)):
    for command in ("devices", "gpu-smoke"):
        r = run([command, "--backend", unavailable])
        assert r.returncode == 2 and "not built" in r.stderr and not r.stdout, r

if a.hardware and a.backend == "cuda" and not os.environ.get("CUDA_VISIBLE_DEVICES") and len(inventory["devices"]) >= 2:
    # Native ordinals change after filtering. Selection must retain runtime UUID
    # identity and must reject an ordinal outside the now-visible device count.
    env = dict(os.environ, CUDA_VISIBLE_DEVICES="1,0")
    remapped = run(["devices", "--backend", "cuda"], env)
    assert remapped.returncode == 0, remapped
    devices = json.loads(remapped.stdout)["devices"]
    assert [d["uuid"] for d in devices] == [inventory["devices"][1]["uuid"], inventory["devices"][0]["uuid"]]
    result = run(["gpu-smoke", "--backend", "cuda", "--device", "1", "--steps", "129"], env)
    assert result.returncode == 0 and json.loads(result.stdout)["uuid"] == devices[1]["uuid"], result
    result = run(["gpu-smoke", "--backend", "cuda", "--device", "2"], env)
    assert result.returncode == 2 and "not visible" in result.stderr and not result.stdout, result
