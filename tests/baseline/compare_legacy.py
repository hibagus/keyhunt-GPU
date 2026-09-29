#!/usr/bin/env python3
"""Compare deterministic legacy CLI results before and after a mechanical move."""
import argparse
import hashlib
import json
from pathlib import Path
from run_cpu_baseline import cases, execute


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    before, after = args.before.resolve(strict=True), args.after.resolve(strict=True)
    selected = {'xpoint_aligned_boundary', 'xpoint_no_match', 'xpoint_above_64_bits',
                'address_compressed', 'address_uncompressed', 'rmd160_compressed',
                'rmd160_uncompressed', 'ethereum_address', 'vanity_prefix',
                'bsgs_known_match_compressed', 'bsgs_known_match_uncompressed',
                'bsgs_no_match', 'minikey_invalid_length'}
    records = []
    for case in cases():
        if case['name'] not in selected:
            continue
        old = execute(before, case['args'], case['data'], case['timeout'])
        new = execute(after, case['args'], case['data'], case['timeout'])
        passed = (not old['timed_out'] and not new['timed_out'] and
                  old['exit_code'] == new['exit_code'] and old['results'] == new['results'])
        # The unchanged legacy parser rejects this valid uncompressed vector.
        # Record that specific defect; do not accept arbitrary matching errors.
        if case['name'] == 'bsgs_known_match_uncompressed':
            passed = passed and old['exit_code'] == 1 and not old['results'] and all(
                'Not lie on elliptic curve' in run['output'] for run in [old, new])
        elif case['keys']:
            passed = passed and bool(old['results'])
        else:
            passed = passed and old['exit_code'] == case['code']
        records.append({'name': case['name'], 'passed': passed, 'before': old, 'after': new})
        print(('PASS' if passed else 'FAIL') + ' ' + case['name'], flush=True)
    report = {'before_sha256': hashlib.sha256(before.read_bytes()).hexdigest(),
              'after_sha256': hashlib.sha256(after.read_bytes()).hexdigest(), 'cases': records}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if len(records) == len(selected) and all(r['passed'] for r in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
