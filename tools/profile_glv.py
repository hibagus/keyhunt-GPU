#!/usr/bin/env python3
"""Capture paired direct/GLV/stepped measurements from the native GPU benchmark."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark', type=Path, required=True)
    parser.add_argument('--keyhunt', type=Path, required=True)
    parser.add_argument('--backend', choices=('hip', 'cuda'), required=True)
    parser.add_argument('--device', type=int, default=0)
    parser.add_argument('--steps', type=int, default=65536)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Run on an otherwise idle device. Inventory is collected before measurement;
    # it must not force context creation or synchronization inside a timed sample.
    inventory = json.loads(subprocess.check_output(
        [str(args.keyhunt.resolve()), 'devices', '--backend', args.backend], text=True))
    measured = json.loads(subprocess.check_output(
        [str(args.benchmark.resolve()), str(args.device), str(args.steps)], text=True))
    medians = []
    for work in measured['workloads']:
        values = {kernel: {metric: statistics.median(sample[metric] for sample in work['samples']
                  if sample['kernel'] == kernel) for metric in ('kernel_ms', 'wall_ms')}
                  for kernel in ('direct', 'glv', 'stepped')}
        medians.append(dict(family=work['family'], region=work['region'], medians=values,
                            direct_over_glv=values['direct']['kernel_ms']/values['glv']['kernel_ms'],
                            glv_over_stepped=values['glv']['kernel_ms']/values['stepped']['kernel_ms']))
    report = dict(passed=True, backend=args.backend, inventory=inventory, measurement=measured,
                  summary=medians, binaries={str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
                                            for path in (args.benchmark, args.keyhunt)},
                  scope='single device, warm volatile unit-stride search, two boundary targets per encoding; no fleet or durable throughput claim')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    for work in medians:
        print(f"{work['family']:8} {work['region']:6} direct/glv={work['direct_over_glv']:.3f} glv/stepped={work['glv_over_stepped']:.3f}")


if __name__ == '__main__':
    main()
