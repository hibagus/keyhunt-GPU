#!/usr/bin/env python3
"""Differential test of the host range contract against Python's exact integers."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import subprocess
import time

MAX = (1 << 256) - 1
ORDER = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
SEED = 0xC05E7AC7


def canonical(value):
    return f"0x{value:064x}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    rng = random.Random(SEED)
    cases = []

    def add(category, operation, values, expected):
        command = operation + " " + " ".join(canonical(x) for x in values)
        answer = "error" if expected is None else "ok " + " ".join(canonical(x) for x in expected)
        cases.append((category, command, answer))

    def arithmetic(category, a, b):
        add(category, "add", (a, b), [a+b] if a+b <= MAX else None)
        add(category, "sub", (a, b), [a-b] if a >= b else None)
        add(category, "mul", (a, b), [a*b] if a*b <= MAX else None)
        add(category, "div", (a, b), divmod(a, b) if b else None)

    for a in range(32):
        for b in range(32):
            arithmetic("exhaustive_small_arithmetic", a, b)
    edges = {0, 1, MAX, ORDER-1, ORDER, ORDER+1}
    for bit in (8, 31, 32, 33, 63, 64, 65, 127, 128, 129, 191, 192, 224, 255):
        edges.update(((1 << bit)-1, 1 << bit, (1 << bit)+1))
    for a in sorted(edges):
        for b in sorted(edges):
            arithmetic("wide_arithmetic_boundaries", a, b)
    for _ in range(2000):
        # Mix short operands (successful wide products) with full-width values
        # (overflow rejection), rather than testing almost exclusively overflow.
        a = rng.getrandbits(rng.choice((32, 64, 128, 192, 256)))
        b = rng.getrandbits(rng.choice((32, 64, 128, 192, 256)))
        arithmetic("random_wide_arithmetic", a, b)
    for a in range(17):
        for b in range(17):
            add("exhaustive_small_intervals", "range", (a, b), [b-a] if 1 <= a < b <= ORDER else None)
    for _ in range(1000):
        a, b = sorted((rng.randrange(1, ORDER), rng.randrange(1, ORDER+1)))
        add("random_wide_intervals", "range", (a, b), [b-a] if a < b else None)
    for a, b in ((0, ORDER), (1, ORDER), (ORDER-1, ORDER), (1, ORDER+1), (ORDER, ORDER), (MAX, 1)):
        add("scalar_domain_boundaries", "range", (a, b), [b-a] if 1 <= a < b <= ORDER else None)

    started = time.monotonic()
    result = subprocess.run([str(binary)], input="\n".join(x[1] for x in cases)+"\n",
                            text=True, capture_output=True, timeout=90)
    lines = result.stdout.splitlines()
    failures = []
    if result.returncode or result.stderr or len(lines) != len(cases):
        failures.append({"exit_code": result.returncode, "stderr": result.stderr,
                         "expected_lines": len(cases), "actual_lines": len(lines)})
    for (_, command, expected), actual in zip(cases, lines):
        if expected != actual:
            failures.append({"command": command, "expected": expected, "actual": actual})
            if len(failures) >= 10:
                break
    report = {"seed": SEED, "binary": str(binary),
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "cases": len(cases), "categories": dict(Counter(x[0] for x in cases)),
              "wall_seconds": round(time.monotonic()-started, 3), "failures": failures}
    args.report.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
