#!/usr/bin/env python3
"""Repeat a finite synthetic CPU workload; report end-to-end wall time only.

Includes process startup, target/table setup, result I/O and CLI shutdown polling.
No application 'keys/s' counters are treated as exact unique coverage.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'baseline'))
from run_cpu_baseline import execute, key_list, search_args, targets  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--mode', choices=['xpoint', 'address', 'rmd160', 'bsgs'], default='xpoint')
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--range-bits', type=int, default=24)
    parser.add_argument('--timeout', type=float, default=120.0)
    args = parser.parse_args()
    if not 20 <= args.range_bits <= 28 or not 1 <= args.samples <= 20 or args.timeout <= 0:
        parser.error('use 20..28 range bits, 1..20 samples and a positive timeout')
    binary = args.binary.resolve(strict=True)
    start, width = 0x100000, 1 << args.range_bits
    # Scalar 1 is below every tested range; no result-writing cost is expected.
    field = {'xpoint': 'x', 'address': 'address', 'rmd160': 'hash160', 'bsgs': 'public_key'}[args.mode]
    argv = search_args(args.mode, f'{start:x}:{start + width:x}', '1048576',
                       extra=['-l', 'compress'])
    runs = []
    for index in range(args.samples + 1):
        run = execute(binary, argv, targets(['1'], field), args.timeout)
        run['warmup'] = index == 0
        run['valid'] = (run['exit_code'] == 0 and not run['timed_out'] and
                        not run['results'] and not key_list(run['output']) and '\nEnd\n' in run['output'])
        runs.append(run)
        print(f"{'warmup' if index == 0 else 'sample ' + str(index)}: "
              f"{run['wall_seconds']:.6f}s valid={run['valid']}", flush=True)
        if not run['valid']:
            break
    samples = [r['wall_seconds'] for r in runs if not r['warmup'] and r['valid']]
    valid = all(r['valid'] for r in runs) and len(samples) == args.samples
    report = {'schema_version': 1, 'captured_at': datetime.now(timezone.utc).isoformat(),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'mode': args.mode, 'thread_count': 1, 'range_start': f'{start:x}',
              'range_end_exclusive_requested': f'{start + width:x}', 'nominal_range_width': str(width),
              'metric': 'process_wall_seconds', 'cache': 'fresh temporary directory per invocation',
              'valid': valid, 'median_seconds': statistics.median(samples) if samples else None,
              'minimum_seconds': min(samples) if samples else None,
              'maximum_seconds': max(samples) if samples else None, 'runs': runs}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
