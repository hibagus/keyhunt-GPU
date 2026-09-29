#!/usr/bin/env python3
"""Run the documented finite CPU examples without touching user result files."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests' / 'baseline'))
from run_cpu_baseline import execute, key_list, VECTORS  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    address = execute(binary, ['-m', 'address', '-f', 'targets.txt', '-r', '1:401',
                              '-n', '1024', '-l', 'compress', '-t', '1', '-q', '-s', '0'],
                      (ROOT / 'tests/1to32.txt').read_text())
    address_keys = key_list(address['output'])
    address_result = address['results'].get('KEYFOUNDKEYFOUND.txt', '')
    address['passed'] = (address['exit_code'] == 0 and not address['timed_out'] and
                         '1' in address_keys and all(1 <= int(k, 16) < 0x401 for k in address_keys) and
                         key_list(address_result) == address_keys and
                         VECTORS['1']['compressed']['address'] in address_result)
    bsgs = execute(binary, ['-m', 'bsgs', '-f', 'targets.txt', '-r', '100000:300000',
                           '-n', '1048576', '-t', '1', '-q', '-s', '0'],
                   VECTORS['100001']['compressed']['public_key'] + '\n')
    bsgs['passed'] = (bsgs['exit_code'] == 1 and not bsgs['timed_out'] and
                      key_list(bsgs['output']) == ['100001'] and
                      key_list(bsgs['results'].get('KEYFOUNDKEYFOUND.txt', '')) == ['100001'])
    report = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'address_quickstart': address, 'bsgs_quickstart': bsgs}
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    for name, result in [('address', address), ('bsgs', bsgs)]:
        print(('PASS' if result['passed'] else 'FAIL') + ' ' + name + ' quickstart')
    return 0 if address['passed'] and bsgs['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
