#!/usr/bin/env python3
"""Supervisor process isolation plus deterministic watchdog/control deadlines."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("supervisor", ROOT / "tools/coordinator_worker.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

# Explicit time injection proves a pause remains safe for arbitrarily long
# durations, while diagnostics and repeated counters never mask a stuck batch.
device = module.Device("0", "0", child=object(), state="running", last_progress=10)
device.event({"type": "progress", "sequence": 1}, 20)
device.event({"type": "progress", "sequence": 1}, 100)
device.event({"message": "diagnostic log growth"}, 100)
assert device.stalled(81, 60)
device.event({"type": "control", "state": "paused"}, 82)
assert not device.stalled(1000000, 60)
device.event({"type": "control", "state": "running"}, 1000001)
assert not device.stalled(1000002, 60)
assert device.stalled(1000062, 60)
device.event({"type": "blocked", "reason": "server-paused"}, 1000063)
assert not device.stalled(2000000, 60)

# An uninterruptible driver PID can survive SIGKILL. Model that OS behavior and
# prove the fleet shares 30 + 5 seconds, independent of its device count.
clock = [0.0]
real_clock = module.time.monotonic
module.time.monotonic = lambda: clock[0]
class Unreapable:
    def poll(self): return None
    def terminate(self): pass
    def kill(self): pass
    def wait(self, timeout):
        clock[0] += timeout
        raise subprocess.TimeoutExpired("kernel-held-owner", timeout)
try:
    children = [Unreapable() for _ in range(64)]
    assert module.stop_children(children) == children
    assert clock[0] == 35, "shutdown deadline multiplied by GPU count"
finally:
    module.time.monotonic = real_clock

FAKE = r'''#!/usr/bin/env python3
import json, pathlib, sys, time
args = dict(zip(sys.argv[2::2], sys.argv[3::2]))
root = pathlib.Path(args["--state-dir"])
action = sys.argv[1]
def emit(**event): print(json.dumps(event), flush=True)
if action == "configuration":
    emit(jobs=[dict(devices=["0", "1"])])
elif action == "status":
    emit(sync_due_in=7200, queues=[])
elif action == "scheduled-sync":
    path = root / "sync-count"
    path.write_text(str(int(path.read_text()) + 1) if path.exists() else "1")
    emit(sent=False)
elif action == "run-device":
    queue = args["--queue"]
    (root / ("started-" + queue)).write_text(str(time.monotonic()))
    deadline = time.monotonic() + 10
    while not all((root / ("started-" + q)).exists() for q in ("0", "1")):
        if time.monotonic() > deadline: raise RuntimeError("supervisor serialized devices")
        time.sleep(.01)
    emit(type="ready", uuid="gpu" + queue, self_test=dict(passed=True))
    emit(type="control", state="running")
    if queue == "0" and (root / "fail-one").exists():
        emit(error="injected device memory exhaustion")
        sys.exit(2)
    assert args["--host-memory"] == "50", "aggregate host budget not divided"
    if queue == "0":
        for _ in range(180): emit(diagnostic="x" * 60000)
    for sequence in range(1, 5):
        time.sleep(.05)
        emit(type="progress", sequence=sequence)
    emit(type="grant-finish", complete=True)
    emit(type="control", state="stopped")
    emit(type="exit", reason="drained")
'''
with tempfile.TemporaryDirectory(prefix="kh-c20-supervisor-") as directory:
    root = Path(directory)
    fake = root / "worker"
    fake.write_text(FAKE)
    fake.chmod(0o700)
    for fault in (False, True):
        state = root / str(fault)
        state.mkdir(mode=0o700)
        if fault:
            (state / "fail-one").touch()
        result = subprocess.run([sys.executable, str(ROOT / "tools/coordinator_worker.py"),
            "--state-dir", str(state), "--worker", str(fake), "--keyhunt", str(fake),
            "--host-memory-total", "100", "--once"], capture_output=True, text=True, timeout=30)
        assert (result.returncode != 0) == fault, (result.stdout, result.stderr)
        if not fault:
            assert (state / "execution-0.log.1").exists(), "long-running diagnostic log did not rotate"
            assert all(path.stat().st_size <= 8 * 1024 * 1024 for path in state.glob("execution-0.log*"))
        saved = json.loads((state / "supervisor.json").read_text())
        assert saved["devices"]["1"]["completed"] == 1
        assert saved["failures"].get("1", 0) == 0
        assert saved["failures"].get("0", 0) == (3 if fault else 0)
        assert (state / "sync-count").read_text() == "1", "per-device/completion synchronization"
        assert len(json.loads((state / "self-tests.json").read_text())) == 2
        assert saved["devices"]["0"]["state"] == ("quarantined" if fault else "stopped")
print("Concurrent children, isolated quarantine, bounded retries, one sync and pause-aware watchdog passed")
