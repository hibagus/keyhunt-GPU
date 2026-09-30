#!/usr/bin/env python3
"""Real Linux signals/socket controls with a bounded CPU executor fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--driver", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
args = parser.parse_args()
report = {"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "cases": []}

with tempfile.TemporaryDirectory(prefix="kh-c14-control-") as temporary:
    root = Path(temporary)
    state = root / "state"
    def cli(family, action, *options, ok=True, directory=None):
        result = subprocess.run([str(args.binary), family, action, "--state-dir", str(directory or state),
                                 *map(str, options)], capture_output=True, text=True, timeout=15)
        assert (result.returncode == 0) == ok, (result.stdout, result.stderr)
        return json.loads(result.stdout) if result.stdout else result.stderr

    def wire(code):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(3)
            client.connect(str(state / "control.sock"))
            client.sendall(code)
            return json.loads(client.recv(2048))

    def until(predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.02)
        raise AssertionError("control condition timed out")

    def activity(expected):
        def query():
            try:
                result = wire(b"?")
                return result if result["state"] == expected else None
            except (FileNotFoundError, ConnectionRefusedError, ConnectionResetError):
                return None
        return until(query)

    def rows(path):
        return [json.loads(line) for line in path.read_text().splitlines()]

    def start(label, delay=100):
        output = root / (label + ".out")
        error = root / (label + ".err")
        with output.open("w") as out, error.open("w") as err:
            process = subprocess.Popen([str(args.driver), str(state), str(delay)], stdout=out, stderr=err)
        return process, output, error

    def cleanup(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)

    process, output, error = start("graceful")
    try:
        activity("running")
        until(lambda: any(r["type"] == "submitted" for r in rows(output)))
        assert (state / "control.sock").stat().st_mode & 0o777 == 0o600
        fixture = rows(output)[0]
        scope = ["--project", fixture["project"], "--job", fixture["job"], "--block", "0"]
        started = time.monotonic()
        ack = cli("checkpoint", "pause")
        assert ack["accepted"] and ack["state"] == "draining" and not ack["durably_paused"]
        paused = activity("paused")
        elapsed = (time.monotonic() - started) * 1000
        assert paused["durably_paused"]
        saved = cli("state", "block", *scope)
        assert saved["state"] == "in_progress" and saved["started"] and saved["covered"]
        time.sleep(0.15)
        assert cli("state", "block", *scope) == saved, "paused frontier changed"
        assert wire(b"invalid")["accepted"] is False
        assert cli("checkpoint", "pause")["durably_paused"], "pause retry not idempotent"

        # Silent/disconnected clients cannot block the owner or raise SIGPIPE.
        silent = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        silent.connect(str(state / "control.sock"))
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as gone:
            gone.connect(str(state / "control.sock"))
            gone.sendall(b"?")
        assert cli("checkpoint", "status")["durably_paused"]
        silent.close()

        snapshot = root / "snapshot"
        cli("state", "backup", "--destination", snapshot)
        cli("state", "check", directory=snapshot)
        assert cli("state", "block", *scope, directory=snapshot)["covered"] == saved["covered"]

        process.send_signal(signal.SIGUSR2)
        activity("running")
        process.send_signal(signal.SIGUSR1)
        activity("paused")
        assert cli("checkpoint", "resume")["accepted"]
        activity("running")
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=10) == 0, error.read_text()
        assert rows(output)[-1]["complete"] is False
        assert not (state / "control.sock").exists()
        cli("state", "check")
        cli("checkpoint", "status", ok=False)
        report["cases"].append({"case": "commands-signals-idle-backup", "pause_request_to_status_ms": elapsed,
                                "owner_flush_ms": paused["pause_ms"], "summary": rows(output)[-1]})
    finally:
        cleanup(process)

    # Separate SIGINT coverage; stop is also available as a local command.
    for label, termination in (("sigint", signal.SIGINT), ("stop-command", None)):
        process, output, error = start(label)
        try:
            activity("running")
            if termination:
                process.send_signal(termination)
            else:
                assert cli("checkpoint", "stop")["accepted"]
            assert process.wait(timeout=10) == 0, error.read_text()
            assert rows(output)[-1]["complete"] is False
            cli("state", "check")
            report["cases"].append({"case": label, "summary": rows(output)[-1]})
        finally:
            cleanup(process)

    # Two delivered interrupts during one admitted batch bypass graceful commit
    # at the next owner boundary. The stale endpoint is replaced only under lock.
    process, output, error = start("forced", 700)
    try:
        until(lambda: any(r["type"] == "submitted" for r in rows(output)))
        process.send_signal(signal.SIGINT)
        time.sleep(0.05)  # standard signals may coalesce if sent simultaneously
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=10) == 128 + signal.SIGINT, error.read_text()
        fixture = rows(output)[0]
        scope = ["--project", fixture["project"], "--job", fixture["job"], "--block", "0"]
        saved = cli("state", "block", *scope)
        assert saved["covered"] == [], "forced exit unexpectedly committed pending coverage"
        assert (state / "control.sock").exists()
        cli("checkpoint", "status", ok=False)
        cli("state", "check")
        report["cases"].append({"case": "second-interrupt", "exit_code": process.returncode})
    finally:
        cleanup(process)
    process, output, error = start("stale-restart")
    try:
        activity("running")
        cli("checkpoint", "stop")
        assert process.wait(timeout=10) == 0, error.read_text()
        assert not (state / "control.sock").exists()
        report["cases"].append({"case": "stale-endpoint-restart"})
    finally:
        cleanup(process)

    # Signals can be delivered on a HIP helper thread rather than the owner.
    process, output, error = start("worker-thread-signals", "threaded")
    try:
        assert process.wait(timeout=10) == 0, error.read_text()
        notices = rows(output)
        assert [r["state"] for r in notices if r["type"] == "control"] == ["draining", "paused", "running", "draining", "stopped"]
        assert notices[-1]["launches"] == 2 and notices[-1]["checkpoints"] == 2
        cli("state", "check")
        report["cases"].append({"case": "worker-thread-signals", "summary": notices[-1]})
    finally:
        cleanup(process)

    # Never overwrite an unexpected regular file at the endpoint.
    (state / "control.sock").write_text("keep me")
    process, output, error = start("unsafe")
    try:
        assert process.wait(timeout=10) == 2
        assert "unsafe checkpoint control socket" in error.read_text()
        assert (state / "control.sock").read_text() == "keep me"
        report["cases"].append({"case": "unsafe-endpoint-rejected"})
    finally:
        cleanup(process)

report["passed"] = True
args.report.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
