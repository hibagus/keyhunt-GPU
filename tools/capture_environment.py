#!/usr/bin/env python3
"""Capture allowlisted build/hardware metadata; never dump environment variables."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def probe(argv, timeout=20):
    executable = shutil.which(argv[0])
    if not executable:
        return {'argv': argv, 'available': False}
    try:
        result = subprocess.run([executable, *argv[1:]], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=timeout,
                                cwd=ROOT)
        return {'argv': argv, 'available': True, 'exit_code': result.returncode,
                'output': result.stdout.strip()}
    except subprocess.TimeoutExpired:
        return {'argv': argv, 'available': True, 'error': 'timeout'}


def collect(binaries):
    cpu = probe(['lscpu', '-J'])
    cpu_fields = {'Architecture:', 'CPU(s):', 'Model name:', 'Thread(s) per core:',
                  'Core(s) per socket:', 'Socket(s):', 'NUMA node(s):', 'Flags:'}
    if cpu.get('exit_code') == 0:
        cpu['fields'] = {e['field'].rstrip(':'): e['data']
                         for e in json.loads(cpu.pop('output'))['lscpu']
                         if e['field'] in cpu_fields}
    rocm = probe(['rocminfo'])
    if rocm.get('exit_code') == 0:
        output = rocm.pop('output')
        agents = []
        for section in re.split(r'(?m)^Agent\s+\d+\s*$', output)[1:]:
            name = re.search(r'(?m)^\s+Name:\s+(gfx\S+)', section)
            if not name:
                continue
            agent = {'isa': name.group(1)}
            for field, label in [('Marketing Name', 'name'), ('Compute Unit', 'compute_units'),
                                 ('Wavefront Size', 'wavefront_size')]:
                value = re.search(r'^\s+' + field + r':\s+([^\n]+)', section, re.M)
                if value:
                    agent[label] = value.group(1).strip()
            agents.append(agent)
        rocm['logical_gpu_agents'] = agents
        rocm['logical_gpu_count'] = len(agents)
        rocm['physical_gpu_count'] = None
    versions = {name: probe(command) for name, command in {
        'gcc': ['gcc', '--version'], 'g++': ['g++', '--version'],
        'cmake': ['cmake', '--version'], 'make': ['make', '--version'],
        'hip': ['/opt/rocm/bin/hipcc', '--version'], 'cuda': ['nvcc', '--version'],
        'openssl': ['openssl', 'version'],
    }.items()}
    version_file = Path('/opt/rocm/core-10.0/.info/version')
    return {
        'schema_version': 1, 'captured_at': datetime.now(timezone.utc).isoformat(),
        'repository_head': probe(['git', 'rev-parse', 'HEAD']).get('output'),
        'tracked_changes': bool(probe(['git', 'diff', 'HEAD', '--name-only']).get('output')),
        'platform': {'system': platform.system(), 'release': platform.release(),
                     'machine': platform.machine(), 'python': platform.python_version(),
                     'allowed_cpu_count': len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count()},
        'cpu': cpu, 'tools': versions,
        'rocm_core_version': version_file.read_text().strip() if version_file.is_file() else None,
        'gpu': rocm, 'gpu_zero_partition': probe(['amd-smi', 'static', '--partition', '--vram', '--gpu', '0', '--json']),
        'binaries': [{'name': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                     for p in binaries],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--binary', type=Path, action='append', default=[])
    args = parser.parse_args()
    args.output.write_text(json.dumps(collect(args.binary), indent=2) + '\n')
    print(f'Environment written to {args.output}')
