#!/usr/bin/env python3
"""Opt-in C12 sparse-journal measurement; synthetic jobs, external temporary state."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--report", required=True, type=Path)
parser.add_argument("--repeats", type=int, default=3)
args = parser.parse_args()
if not 1 <= args.repeats <= 20:
    parser.error("repeats must be 1..20")
binary = args.binary.resolve()
cases = []
# Compare a modest job and a 255-bit stress job without enumerating either.
for bits in (20, 255):
    for policy in ("sequential", "random", "random-window"):
        samples = []
        for repeat in range(args.repeats):
            with tempfile.TemporaryDirectory(prefix="keyhunt-c12-measure-") as temporary:
                state = Path(temporary) / "state"

                def run(action, *options):
                    start = time.perf_counter_ns()
                    result = subprocess.run(
                        [str(binary), "state", action, "--state-dir", str(state), *map(str, options)],
                        text=True, capture_output=True, timeout=180, check=True)
                    return json.loads(result.stdout), (time.perf_counter_ns() - start) / 1e6

                project = run("project-create", "--name", "Synthetic scaling")[0]["project"]
                job = run("job-create", "--project", project, "--mode", "xpoint",
                          "--range", "1:" + hex(2**bits + 1), "--block-width", "1",
                          "--target-digest", "11" * 32, "--algorithm-digest", "22" * 32,
                          "--seed", "33" * 32)[0]["job"]
                scope = ["--project", project, "--job", job]
                empty = run("inspect", *scope)[0]
                assert empty["tree_nodes"] == 0 and int(empty["blocks"], 16) == 2**bits
                response, claim_ms = run("claim", *scope, "--owner", "measure", "--request", "batch",
                                        "--policy", policy, "--count", "256", "--lifetime", "2592000",
                                        *(["--window", "1000"] if policy == "random-window" else []))
                ids = [int(g["block"], 16) for g in response["assignments"]]
                assert len(ids) == len(set(ids)) == 256
                if policy == "sequential":
                    assert ids == list(range(256))
                if policy == "random-window":
                    assert len({i // 4096 for i in ids}) == 1
                claimed = run("inspect", *scope)[0]
                assert int(claimed["unexplored"], 16) == 2**bits - 256
                _, audit_ms = run("check")
                _, compact_ms = run("compact")
                compacted = run("inspect", *scope)[0]
                samples.append({"claim_wall_ms": claim_ms, "audit_wall_ms": audit_ms,
                                "compact_wall_ms": compact_ms, "empty": empty,
                                "claimed": claimed, "compacted": compacted})
        cases.append({"block_count": hex(2**bits), "policy": policy, "claim_count": 256,
                      "median_claim_wall_ms": statistics.median(s["claim_wall_ms"] for s in samples),
                      "samples": samples})
report = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
          "platform": platform.platform(), "repeats": args.repeats,
          "timing_scope": "CLI process startup, open/validation, transaction+FULL commit, JSON and close/checkpoint",
          "storage_scope": "one synthetic project/job per fresh external temporary directory; no GPU execution",
          "cases": cases}
args.report.write_text(json.dumps(report, indent=2) + "\n")
for case in cases:
    sample = case["samples"][0]
    print(case["block_count"], case["policy"], round(case["median_claim_wall_ms"], 2),
          sample["claimed"]["tree_nodes"], sample["compacted"]["database_bytes"])
