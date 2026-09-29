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
    options = parser.parse_args()
    binary = options.binary.resolve()
    rng = random.Random(SEED)
    cases = []

    def add(category, operation, values, expected):
        command = operation + " " + " ".join(canonical(x) for x in values)
        if expected is None:
            answer = "error"
        elif expected == "none":
            answer = "ok none"
        else:
            answer = "ok " + " ".join(canonical(x) for x in expected)
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

    def grid_case(category, begin, end, width, index):
        count = (end-begin+width-1)//width if width and 1 <= begin < end <= ORDER else 0
        expected = None
        if index < count:
            start = begin+index*width
            stop = min(end, start+width)
            expected = (count, start, stop)
            for scalar in (start, stop-1):
                add(category, "locate", (begin, end, width, scalar), [index])
        add(category, "grid", (begin, end, width, index), expected)

    for begin in range(1, 9):
        for end in range(begin+1, 17):
            for width in range(1, 20):
                count = (end-begin+width-1)//width
                for index in range(count+1):
                    grid_case("exhaustive_small_grids", begin, end, width, index)
    for _ in range(2000):
        begin = rng.randrange(1, ORDER)
        end = rng.randrange(begin+1, ORDER+1)
        width = rng.randrange(1, 1 << rng.choice((1, 32, 64, 128, 192, 256)))
        count = (end-begin+width-1)//width
        for index in (0, count-1, rng.randrange(count), count):
            grid_case("random_wide_grids", begin, end, width, index)
    for begin, end, width in ((1, ORDER, 1), (1, ORDER, MAX),
                              (ORDER-3, ORDER, 2), (1 << 200, (1 << 200)+100, 32),
                              (1, ORDER, 0), (0, 10, 1), (1, ORDER+1, 1)):
        count = (end-begin+width-1)//width if width else 1
        for index in (0, max(0, count-1), count, MAX):
            grid_case("block_grid_boundaries", begin, end, width, index)
    for scalar in (0, ORDER, MAX):
        add("block_grid_boundaries", "locate", (1, ORDER, 1, scalar), None)

    local_max = (1 << 64)-1

    def work_case(category, begin, end, width, index, cursor, limit):
        count = (end-begin+width-1)//width
        start, stop = begin+index*width, min(end, begin+(index+1)*width)
        expected = None
        if index < count and 0 < limit <= local_max and start <= cursor <= stop:
            expected = "none" if cursor == stop else (cursor, min(stop, cursor+limit), min(stop-cursor, limit))
        args = (begin, end, width, index, cursor, limit)
        add(category, "work", args, expected)
        return args, expected

    def batch_case(category, work_args, work_result, cursor, limit, index):
        expected = None
        if work_result == "none":
            expected = "none"
        elif work_result is not None:
            start, stop, _ = work_result
            if 0 < limit <= local_max and start <= cursor <= stop:
                if cursor == stop:
                    expected = "none"
                else:
                    count = min(stop-cursor, limit)
                    if 0 <= index < count:
                        expected = (cursor, cursor+count, count, cursor+index)
        add(category, "batch", (*work_args, cursor, limit, index), expected)

    for begin in range(1, 4):
        for end in range(begin+1, 10):
            for width in range(1, 7):
                count = (end-begin+width-1)//width
                for index in range(count):
                    start, stop = begin+index*width, min(end, begin+(index+1)*width)
                    for cursor in range(start-1, stop+2):
                        for limit in range(5):
                            args, expected = work_case("exhaustive_small_work", begin, end, width, index, cursor, limit)
                            if expected is not None and expected != "none":
                                for batch_cursor in range(expected[0]-1, expected[1]+2):
                                    for batch_limit in range(4):
                                        batch_case("exhaustive_small_batches", args, expected,
                                                   batch_cursor, batch_limit, 0)

    for _ in range(1500):
        begin = rng.randrange(1, ORDER)
        end = rng.randrange(begin+1, ORDER+1)
        width = rng.randrange(1, 1 << rng.choice((1, 64, 128, 256)))
        count = (end-begin+width-1)//width
        index = rng.randrange(count)
        start, stop = begin+index*width, min(end, begin+(index+1)*width)
        cursor = rng.randrange(start, stop)
        limit = rng.randrange(1, local_max+1)
        args, expected = work_case("random_wide_work", begin, end, width, index, cursor, limit)
        for batch_limit in (1, rng.randrange(1, local_max+1), local_max):
            batch_cursor = rng.randrange(expected[0], expected[1])
            batch_count = min(expected[1]-batch_cursor, batch_limit)
            for local_index in (0, batch_count-1, batch_count):
                batch_case("random_wide_batches", args, expected, batch_cursor, batch_limit, local_index)

    for begin, end, width, index, cursor in (
            (1, ORDER, MAX, 0, 1), (1, ORDER, 1, 1 << 200, (1 << 200)+1),
            (ORDER-3, ORDER, MAX, 0, ORDER-3), (ORDER-3, ORDER, 2, 1, ORDER-1),
            (ORDER-3, ORDER, 2, 1, ORDER), (1, ORDER, 1, ORDER-1, ORDER)):
        for limit in (0, 1, local_max, local_max+1):
            args, expected = work_case("work_boundaries", begin, end, width, index, cursor, limit)
            if expected is not None and expected != "none":
                for batch_cursor in (expected[0], expected[1]):
                    for batch_limit in (0, 1, local_max, local_max+1):
                        for local_index in (0, local_max, local_max+1):
                            batch_case("batch_boundaries", args, expected, batch_cursor, batch_limit, local_index)

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
    options.report.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2))
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
