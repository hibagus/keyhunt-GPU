#!/usr/bin/env python3
"""Check discovery errors and JSON identity without starting a search."""
import argparse
import json
import os
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--binary', required=True)
p.add_argument('--hip', action='store_true')
a = p.parse_args()


def run(args, env=None):
    return subprocess.run([a.binary, *args], capture_output=True, text=True,
                          timeout=30, env=env)


for args in [['devices'], ['devices', '--backend', 'cuda'],
             ['devices', '--backend', 'hip', '--unknown']]:
    r = run(args)
    assert r.returncode == 2 and 'usage:' in r.stderr and not r.stdout, r
r = run(['devices', '--backend', 'hip'])
if not a.hip:
    assert r.returncode == 2 and 'not built' in r.stderr and not r.stdout, r
else:
    assert r.returncode == 0, r
    inventory = json.loads(r.stdout)
    assert inventory['backend'] == 'hip'
    seen = set()
    for i, d in enumerate(inventory['devices']):
        assert d['ordinal'] == i and len(d['uuid']) == 32 and d['uuid'] not in seen
        seen.add(d['uuid'])
        assert d['name'] and d['architecture'] and d['pci_bus_id']
        assert 0 <= d['free_memory_bytes'] <= d['total_memory_bytes']
        assert d['compute_units'] > 0 and d['warp_size'] > 0
        assert d['physical_id'] or 'physical package identity unavailable' in d['warnings']
    # No visible devices is valid discovery, but never a fallback CPU search.
    env = dict(os.environ, HIP_VISIBLE_DEVICES='-1', ROCR_VISIBLE_DEVICES='-1')
    r = run(['devices', '--backend', 'hip'], env)
    assert r.returncode == 0 and json.loads(r.stdout)['devices'] == [], r
print('Backend discovery CLI checks passed')
