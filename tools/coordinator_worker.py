#!/usr/bin/env python3
"""Supervise HIP execution and one persisted machine synchronization schedule.

Network I/O and GPU ownership live in separate child processes. The supervisor
never initializes HIP, resets devices, or contacts HTTPS on a per-batch path.
C15 keeps C14's single local executor; simultaneous device execution is C20.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import time


def private_write(path, value):
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as output:
        output.write(value)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--keyhunt", type=Path, required=True)
    parser.add_argument("--table", type=Path)
    parser.add_argument("--once", action="store_true", help="consume saved queues, then exit without an extra sync")
    parser.add_argument("--retry-failed", action="store_true", help="explicitly clear saved device failure counts")
    parser.add_argument("--stall-seconds", type=int, default=300)
    parser.add_argument("--kernel", choices=("direct", "stepped"), default="stepped")
    parser.add_argument("--group-size", choices=("auto", "1", "8"), default="auto")
    parser.add_argument("--batch-size", type=int, default=1048576)
    parser.add_argument("--giant-batch", type=int, default=16384)
    parser.add_argument("--target-batch", type=int, default=64)
    parser.add_argument("--host-memory", type=int, default=1073741824)
    args = parser.parse_args()
    if args.stall_seconds < 60:
        parser.error("stall deadline must be at least 60 seconds")
    root, worker, keyhunt = args.state_dir.resolve(), args.worker.resolve(), args.keyhunt.resolve()
    lock = os.open(root / "supervisor.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    info = os.fstat(lock)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_nlink != 1:
        raise RuntimeError("unsafe supervisor lock")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def command(action, *extra):
        return [str(worker), action, "--state-dir", str(root), *map(str, extra)]

    def read(action, *extra):
        result = subprocess.run(command(action, *extra), capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        return json.loads(result.stdout)

    config = read("configuration")
    devices = [device for job in config["jobs"] for device in job["devices"]]
    if any(not device.isdecimal() or str(int(device)) != device for device in devices):
        raise RuntimeError("HIP supervisor device queues must use canonical visible ordinals (0, 1, ...)")
    state_path = root / "supervisor.json"
    saved = json.loads(state_path.read_text()) if state_path.exists() else {}
    failures = {} if args.retry_failed else saved.get("failures", {})
    gpu = network = None
    gpu_log = network_log = None
    paused = stopping = False
    drain_at = None
    gpu_device = None
    last_gpu_output = time.monotonic()
    output_size = 0
    last_status = 0.0
    status = read("status")
    sync_due = time.monotonic() + status["sync_due_in"]
    last_network_error = None
    initial_sync_checked = False
    cursor = 0

    def control(signum, _frame):
        nonlocal paused, stopping, drain_at
        if signum in (signal.SIGINT, signal.SIGTERM):
            stopping = True
            drain_at = time.monotonic()
            forward = signal.SIGTERM
        else:
            paused = signum == signal.SIGUSR1
            forward = signum
        if gpu and gpu.poll() is None:
            gpu.send_signal(forward)

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGUSR1, signal.SIGUSR2):
        signal.signal(sig, control)

    try:
        # Each check runs in a fresh process, before accepting new assignments.
        # There is no persisted pass that could survive a build/runtime change.
        self_tests = {}
        for device in devices:
            result = subprocess.run(command("self-test", "--device", device), capture_output=True, text=True, timeout=300)
            if result.returncode:
                failures[device] = 3
                last_network_error = "device self-test failed: " + result.stderr.strip()
            else:
                self_tests[device] = json.loads(result.stdout)
        private_write(root / "self-tests.json", json.dumps(self_tests) + "\n")
        if len(self_tests) != len(devices):
            raise RuntimeError(last_network_error)
        while True:
            now = time.monotonic()
            if gpu and gpu.poll() is not None:
                if gpu.returncode:
                    failures[gpu_device] = failures.get(gpu_device, 0) + 1
                gpu_log.close()
                gpu = gpu_log = None
                drain_at = None
            if network and network.poll() is not None:
                network_log.close()
                last_network_error = None if network.returncode == 0 else (root / "sync.log").read_text()[-2048:]
                network = network_log = None
                status = read("status")
                sync_due = now + status["sync_due_in"]
                initial_sync_checked = True
            if stopping:
                if not gpu:
                    break
                if drain_at is not None and now - drain_at >= 30:
                    # Preserve unfinished grants after bounded drain. Never reset
                    # a physical GPU or take work owned by another process.
                    gpu.kill()
                    failures[gpu_device] = 3
            else:
                if network is None and (now >= sync_due or not initial_sync_checked):
                    network_log = open(root / "sync.log", "w")
                    os.chmod(root / "sync.log", 0o600)
                    network = subprocess.Popen(command("scheduled-sync"), stdout=network_log, stderr=subprocess.STDOUT)
                if gpu is None and not paused and initial_sync_checked:
                    selected = None
                    for offset in range(len(devices)):
                        index = (cursor + offset) % len(devices)
                        device = devices[index]
                        if failures.get(device, 0) >= 3:
                            continue
                        selected = read("next", "--device", device)
                        if selected:
                            cursor = (index + 1) % len(devices)
                            gpu_device = device
                            break
                    if selected:
                        targets = selected["targets"]
                        width = 64 if selected["mode"] == "xpoint" else 130
                        target_path = root / "execution-targets.txt"
                        private_write(target_path, "\n".join(targets[i:i+width] for i in range(0, len(targets), width)) + "\n")
                        words = [str(keyhunt), "checkpoint", "run", "--state-dir", str(root), "--backend", "hip",
                                 "--grant", selected["token"], "--targets", str(target_path), "--device", gpu_device]
                        if selected["mode"] == "xpoint":
                            words += ["--batch-size", str(args.batch_size), "--kernel", args.kernel]
                        else:
                            if args.table is None:
                                raise RuntimeError("BSGS execution requires --table; the coordinator never holds worker tables")
                            words += ["--table", str(args.table.resolve()), "--group-size", args.group_size,
                                      "--giant-batch", str(args.giant_batch), "--target-batch", str(args.target_batch),
                                      "--host-memory", str(args.host_memory)]
                        gpu_log = open(root / "execution.log", "w")
                        os.chmod(root / "execution.log", 0o600)
                        gpu = subprocess.Popen(words, stdout=gpu_log, stderr=subprocess.STDOUT)
                        last_gpu_output, output_size = now, 0
                    elif args.once and network is None:
                        break
            if gpu and not paused and not stopping:
                size = (root / "execution.log").stat().st_size
                if size != output_size:
                    output_size, last_gpu_output = size, now
                if now - last_gpu_output >= args.stall_seconds:
                    failures[gpu_device] = 3
                    if drain_at is None:
                        gpu.terminate()
                        drain_at = now
                    elif now - drain_at >= 30:
                        gpu.kill()
            if now - last_status >= 1:
                status = read("status")
                private_write(state_path, json.dumps(dict(pid=os.getpid(), paused=paused, stopping=stopping,
                              gpu_pid=gpu.pid if gpu else None, gpu_device=gpu_device,
                              sync_pid=network.pid if network else None, failures=failures,
                              last_network_error=last_network_error, worker=status)) + "\n")
                last_status = now
            time.sleep(.1)
    finally:
        # Every exit path drains/stops children before releasing the supervisor
        # lock; C14's executor lock remains the final defense against overlap.
        for child in (gpu, network):
            if child and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        for log in (gpu_log, network_log):
            if log:
                log.close()
        private_write(state_path, json.dumps(dict(pid=None, paused=paused, stopped=True, failures=failures,
                      last_network_error=last_network_error, worker=read("status"))) + "\n")
        os.close(lock)
    if args.once and last_network_error:
        raise RuntimeError(last_network_error)
    if any(count >= 3 for count in failures.values()):
        raise RuntimeError("device quarantined after failure; inspect execution.log and explicitly retry after repair")


if __name__ == "__main__":
    main()
