#!/usr/bin/env python3
"""Opt-in warm HIP pause latency at small and default bounded launch geometry."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests/oracle"))
from oracle_selftest import check_source, run as oracle_run

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--oracle", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
parser.add_argument("--repeats", type=int, default=5)
args = parser.parse_args()
if not 1 <= args.repeats <= 20:
    parser.error("repeats must be 1..20")
binary = args.binary.resolve()
def invoke(words):
    result = subprocess.run([str(binary), *map(str, words)], text=True, capture_output=True, check=True, timeout=90)
    return [json.loads(line) for line in result.stdout.splitlines()]
report = {"recorded_utc": datetime.now(timezone.utc).isoformat(),
          "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "oracle_commit": check_source(),
          "inventory": invoke(["devices", "--backend", "hip"])[0], "samples": [],
          "scope": "warm single HIP device; external local-socket pause request through durably-paused status; one startup pause excluded; no universal latency bound"}
with tempfile.TemporaryDirectory(prefix="kh-c14-latency-") as temporary:
    root = Path(temporary)
    table = root / "table.khb"
    invoke(["bsgs-table", "build", "--m", "257", "--output", table])
    public = oracle_run(args.oracle, [f"pub {(1 << 80) + i:064x}" for i in range(4)])
    for mode in ("xpoint", "bsgs"):
        targets = root / (mode + ".txt")
        targets.write_text("\n".join(p[2:66] if mode == "xpoint" else p for p in public) + "\n")
        inputs = ["--targets", targets] + (["--table", table] if mode == "bsgs" else [])
        for geometry in ("small", "default"):
            state = root / (mode + "-" + geometry)
            def local(family, action, *words):
                return invoke([family, action, "--state-dir", state, *words])
            project = local("state", "project-create", "--name", "C14 synthetic pause timing")[0]["project"]
            job = local("checkpoint", "create", "--project", project, "--mode", mode,
                        "--range", "1:1000000000001", "--block-width", "1000000000000", *inputs)[0]["job"]
            scope = ["--project", project, "--job", job]
            grant = local("state", "claim", *scope, "--owner", "measure", "--request", "claim")[0]["assignments"][0]["grant"]
            tuning = []
            if geometry == "small":
                tuning = ["--batch-size", "128"] if mode == "xpoint" else ["--giant-batch", "1", "--target-batch", "1"]
            words = ["checkpoint", "run", "--state-dir", state, "--backend", "hip", "--grant", grant, *inputs, *tuning]
            log, err = root / "run.out", root / "run.err"
            with log.open("w") as output, err.open("w") as error:
                process = subprocess.Popen([str(binary), *map(str, words)], stdout=output, stderr=error)
            def wire(code):
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.settimeout(15)
                    client.connect(str(state / "control.sock"))
                    client.sendall(code)
                    return json.loads(client.recv(2048))
            def wait(expected):
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    assert process.poll() is None, err.read_text()
                    try:
                        status = wire(b"?")
                        if status["state"] == expected:
                            return status
                    except (FileNotFoundError, ConnectionRefusedError, ConnectionResetError):
                        pass
                    time.sleep(0.001)
                raise RuntimeError("pause measurement timed out")
            try:
                wait("running")
                samples = []
                for sample in range(-1, args.repeats):
                    # Exclude the first device initialization/drain. Subsequent
                    # pauses retain allocations and measure the warm owner.
                    time.sleep(0.05)
                    start = time.perf_counter_ns()
                    assert wire(b"P")["accepted"]
                    paused = wait("paused")
                    elapsed = (time.perf_counter_ns()-start)/1e6
                    if sample >= 0:
                        samples.append({"request_to_paused_ms": elapsed, "owner_observation_to_paused_ms": paused["pause_ms"]})
                    if sample + 1 < args.repeats:
                        assert wire(b"R")["accepted"]
                        wait("running")
                assert wire(b"T")["accepted"]
                assert process.wait(timeout=30) == 0, err.read_text()
                summary = json.loads(log.read_text().splitlines()[-1])
                assert not summary["complete"] and summary["match_observations"] == 0
                saved = local("state", "block", *scope, "--block", "0")[0]
                covered = sum(int(v["end_exclusive"], 16)-int(v["begin"], 16) for v in saved["covered"])
                assert covered == int(summary["computed_scalars"], 16)
                local("state", "check")
                values = [s["request_to_paused_ms"] for s in samples]
                report["samples"].append({"mode": mode, "geometry": geometry, "options": tuning, "targets": 4,
                    "bsgs_m": 257 if mode == "bsgs" else None, "samples": samples,
                    "median_ms": statistics.median(values), "maximum_ms": max(values), "summary": summary})
                print(f"{mode} {geometry}: median {statistics.median(values):.3f} ms, max {max(values):.3f} ms", flush=True)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=30)
report["passed"] = True
args.report.write_text(json.dumps(report, indent=2) + "\n")
