#!/usr/bin/env python3
"""Exercise the preserved daemon on a loopback-only address in a temporary cwd.

The old daemon binds TCP 8080 regardless of -p; do not run on a shared listener.
The test fails if that address/port is already occupied.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

# A separate loopback address avoids TIME_WAIT conflicts between successive
# before/after tests of the daemon's fixed port. No non-loopback bind is used.
ADDRESS = (f'127.77.{(os.getpid() >> 8) & 255}.{os.getpid() & 255}', 8080)
DATA = json.loads((Path(__file__).resolve().parents[1] / 'baseline/vectors.json').read_text())


def request(process, payload, deadline):
    while True:
        if process.poll() is not None:
            raise RuntimeError(f'daemon exited with {process.returncode}')
        try:
            connection = socket.create_connection(ADDRESS, timeout=1)
            break
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise TimeoutError('daemon did not start listening')
            time.sleep(0.05)
    with connection:
        connection.settimeout(10)
        connection.sendall(payload.encode())
        response = bytearray()
        while True:
            chunk = connection.recv(1024)
            if not chunk:
                return response.decode()
            response.extend(chunk)
            if len(response) > 4096:
                raise RuntimeError('unexpectedly large daemon response')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    # Fail before starting if a listener occupies the test endpoint.
    with socket.socket() as available:
        available.bind(ADDRESS)
    vectors = DATA['vectors']
    checks = [
        ('known_match', vectors['100001']['compressed']['public_key'] + ' 100000:300000\n', '100001'),
        ('no_match', vectors['400000']['compressed']['public_key'] + ' 100000:300000\n', '404 Not Found'),
        ('invalid_request', 'invalid\n', '400 Bad Request'),
    ]
    records = []
    with tempfile.TemporaryDirectory(prefix='keyhunt-bsgsd-') as cwd:
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen([str(binary), '-i', ADDRESS[0], '-n', '1048576', '-t', '1'],
                                       cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
            try:
                for name, payload, expected in checks:
                    response = request(process, payload, time.monotonic() + 30)
                    passed = response == expected
                    records.append({'name': name, 'response': response, 'expected': expected, 'passed': passed})
                    print(('PASS' if passed else 'FAIL') + ' ' + name, flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log.seek(0)
            output = log.read().decode(errors='replace')
    if args.report:
        args.report.write_text(json.dumps({'cases': records, 'output': output}, indent=2) + '\n')
    return 0 if len(records) == len(checks) and all(r['passed'] for r in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
