#!/usr/bin/env python3
"""Exercise extracted CPU loaders and native-cache reuse with synthetic targets."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'baseline'))
from run_cpu_baseline import DATA, VECTORS, execute, key_list, search_args, targets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    results = []
    for mode, extra, text in [
        ('xpoint', [], targets(['1', '2'])),
        ('address', ['-l', 'compress'], targets(['1', '2'], 'address')),
        ('rmd160', ['-l', 'compress'], targets(['1', '2'], 'hash160')),
        ('address', ['-c', 'eth'], DATA['ethereum_scalar_1'] + '\n'),
    ]:
        expected = ['1'] if 'eth' in extra else ['1', '2']
        name = mode + ('_eth' if 'eth' in extra else '') + '_cache_roundtrip'
        with tempfile.TemporaryDirectory(prefix='keyhunt-targets-') as folder:
            cwd = Path(folder)
            (cwd / 'targets.txt').write_text(text)
            command = [str(binary), *search_args(mode, '1:401', extra=extra + ['-S'])]
            first = subprocess.run(command, cwd=cwd, capture_output=True, timeout=15)
            result_file = cwd / 'KEYFOUNDKEYFOUND.txt'
            first_keys = key_list(result_file.read_text()) if result_file.exists() else []
            result_file.unlink(missing_ok=True)
            second = subprocess.run(command, cwd=cwd, capture_output=True, timeout=15)
            second_keys = key_list(result_file.read_text()) if result_file.exists() else []
            cache = list(cwd.glob('data_*.dat'))
            ok = (first.returncode == second.returncode == 0
                  and Counter(first_keys) == Counter(second_keys) == Counter(expected)
                  and len(cache) == 1 and b'Reading file data_' in second.stdout)
            # Corrupt the stored expected digest: failure must not start a search.
            if cache:
                raw = bytearray(cache[0].read_bytes())
                raw[0] ^= 1
                cache[0].write_bytes(raw)
                result_file.unlink(missing_ok=True)
                bad = subprocess.run(command, cwd=cwd, capture_output=True, timeout=15)
                ok = ok and bad.returncode == 1 and b'checksum mismatch' in bad.stderr
                ok = ok and not result_file.exists()
            results.append({'name': name, 'passed': ok, 'first_keys': first_keys,
                            'cached_keys': second_keys})
    mixed = ('05' + '00' * 32 + '\n' + targets(['100001'], 'public_key')
             + targets(['100002'], 'public_key', 'uncompressed'))
    run = execute(binary, search_args('bsgs', '100000:300000', '1048576'), mixed)
    # execute() returns a record; failed parsing must not disturb the two valid entries.
    keys = key_list(run['output'])
    results.append({'name': 'bsgs_mixed_valid_invalid_encodings',
                    'passed': run['exit_code'] == 1 and not run['timed_out']
                    and Counter(keys) == Counter(['100001', '100002'])
                    and Counter(key_list(run['results'].get('KEYFOUNDKEYFOUND.txt', '')))
                    == Counter(keys), 'keys': keys})
    report = {'binary': str(binary), 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'cases': results}
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    for result in results:
        print(('PASS ' if result['passed'] else 'FAIL ') + result['name'])
    return 0 if all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
