#!/usr/bin/env python3
"""Run persistent, isolated GPU owners and one machine synchronization schedule.

Device events describe completed batches and authoritative local control states.
Log growth is never a progress signal. HTTPS runs in a separate bounded child.
"""
import re
import argparse
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import selectors
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


def private_log(path):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    return os.fdopen(fd, "ab", buffering=0)


def stop_children(children, deadline=None):
    """Share drain/kill deadlines even if several kernel-held PIDs never reap."""
    for child in children:
        if child.poll() is None:
            child.terminate()
    if deadline is None:
        deadline = time.monotonic() + 30
    for child in children:
        try:
            child.wait(timeout=max(0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            child.kill()
    reap_deadline = time.monotonic() + 5
    survivors = []
    for child in children:
        try:
            child.wait(timeout=max(0, reap_deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            survivors.append(child)
    return survivors


@dataclass
class Device:
    queue: str
    ordinal: str
    child: object = None
    log: object = None
    log_bytes: int = 0
    buffer: bytes = b""
    state: str = "pending"
    last_progress: float = 0
    terminal_at: object = None
    sequence: int = 0
    drain_at: object = None
    kill_sent: bool = False
    retry_at: float = 0
    done: bool = False
    ready: bool = False
    uuid: str = ""
    completed: int = 0
    grant: object = None
    error: object = None

    def event(self, event, now):
        kind = event.get("type")
        # A terminal notification precedes executor destruction. Keep watching
        # the PID, and never let later log output renew its teardown deadline.
        if self.terminal_at is not None:
            if "error" in event:
                self.error = event["error"]
            return
        if kind == "exit" or (kind == "control" and event.get("state") == "stopped"):
            self.terminal_at, self.state = now, "stopped"
            return
        if kind == "ready":
            self.ready, self.uuid, self.last_progress = True, event["uuid"], now
        elif kind in ("progress", "checkpoint"):
            sequence = event.get("sequence", 0)
            if isinstance(sequence, int) and sequence > self.sequence:
                self.sequence, self.last_progress = sequence, now
        elif kind == "control":
            state = event.get("state")
            if state == "running" and self.state in ("paused", "idle", "pending"):
                self.last_progress = now
            self.state = state
        elif kind == "grant-start":
            self.grant = event["grant"]
        elif kind == "grant-finish" and event.get("complete"):
            self.completed += 1
            self.grant = None
        elif kind == "blocked":
            self.state, self.error = "idle", event.get("reason")
        elif "error" in event:
            self.error = event["error"]

    def stalled(self, now, deadline):
        # Pauses received through the local socket are as authoritative as a
        # supervisor signal. Draining still has a deadline: it may hide a hang.
        if self.child is None:
            return False
        if self.terminal_at is not None:
            return now - self.terminal_at >= deadline
        return self.state not in ("idle", "paused") and now - self.last_progress >= deadline

    def snapshot(self):
        return dict(pid=self.child.pid if self.child else None, ordinal=self.ordinal, uuid=self.uuid,
                    state=self.state, completed=self.completed, grant=self.grant, error=self.error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--keyhunt", type=Path, required=True, help="retained for launcher compatibility and control commands")
    parser.add_argument("--table", type=Path)
    parser.add_argument("--once", action="store_true", help="consume saved queues, then exit without an extra sync")
    parser.add_argument("--retry-failed", action="store_true", help="clear saved device failure counts after repair")
    parser.add_argument("--devices", help="comma-separated configured queues to run; omitted queues keep active ownership")
    parser.add_argument("--device-map", action="append", default=[], metavar="QUEUE=ORDINAL",
                        help="map a stable queue to a currently visible ordinal")
    parser.add_argument("--rebind-device", action="append", default=[], metavar="QUEUE",
                        help="allow a stopped queue to move to a different UUID")
    parser.add_argument("--stall-seconds", type=int, default=300)
    parser.add_argument("--kernel", choices=("direct", "stepped", "glv"), help="scalar search also accepts glv; override the mode default: direct for minikeys, stepped for scalar search")
    parser.add_argument("--batch-order", choices=("forward", "both-ends"), help="Scalar families only: choose batches within each grant")
    parser.add_argument("--ordinal-order", choices=("forward", "reverse", "both-ends", "dance", "random-window"), help="Minikeys only: choose ordinal execution order within each grant")
    parser.add_argument("--ordinal-seed", help="minikey random-window only: hexadecimal 256-bit seed (default zero)")
    parser.add_argument("--ordinal-window", type=int, help="minikey random-window only: shuffle 1..256 ordinal tiles (default 64)")
    parser.add_argument("--tile-order", choices=("forward", "reverse", "both-ends", "dance", "random-window"), help="BSGS only: choose tiles within each grant; saved scalar coverage is unchanged")
    parser.add_argument("--tile-seed", help="random-window only: hexadecimal 256-bit seed (default zero)")
    parser.add_argument("--tile-window", type=int, help="random-window only: shuffle 1..256 tiles (default 64)")
    parser.add_argument("--group-size", choices=("auto", "1", "8"), default="auto")
    parser.add_argument("--batch-size", type=int, default=1048576)
    parser.add_argument("--giant-batch", type=int, default=16384)
    parser.add_argument("--target-batch", type=int, default=64)
    parser.add_argument("--host-memory", type=int, default=1073741824, help="maximum table/target budget per device")
    parser.add_argument("--host-memory-total", type=int, default=8589934592,
                        help="aggregate table/target budget across selected device processes")
    parser.add_argument("--backend", choices=("hip", "cuda"), default="hip")
    args = parser.parse_args()
    if (args.ordinal_seed is not None or args.ordinal_window is not None) and args.ordinal_order != "random-window":
        parser.error("ordinal-seed/ordinal-window require ordinal-order random-window")
    if args.ordinal_window is not None and not 1 <= args.ordinal_window <= 256:
        parser.error("ordinal-window must be 1..256")
    if args.ordinal_seed is not None and not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]{1,64}", args.ordinal_seed):
        parser.error("ordinal-seed must be a hexadecimal 256-bit integer")
    if (args.tile_seed is not None or args.tile_window is not None) and args.tile_order != "random-window":
        parser.error("tile-seed/tile-window require tile-order random-window")
    if args.tile_window is not None and not 1 <= args.tile_window <= 256:
        parser.error("tile-window must be 1..256")
    if args.tile_seed is not None:
        if not re.fullmatch(r"(?:0[xX])?[0-9a-fA-F]{1,64}", args.tile_seed):
            parser.error("tile-seed must be a hexadecimal 256-bit integer")
    if args.stall_seconds < 60 or min(args.host_memory, args.host_memory_total) < 1:
        parser.error("stall deadline must be at least 60 seconds and memory budgets must be positive")
    root, worker = args.state_dir.resolve(), args.worker.resolve()
    lock = os.open(root / "supervisor.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    info = os.fstat(lock)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077 or info.st_nlink != 1:
        raise RuntimeError("unsafe supervisor lock")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def command(action, *extra):
        return [str(worker), action, "--state-dir", str(root), *map(str, extra)]

    def read(action):
        result = subprocess.run(command(action), capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        return json.loads(result.stdout)

    config = read("configuration")
    configured = [device for job in config["jobs"] for device in job["devices"]]
    queues = args.devices.split(",") if args.devices else configured
    if not queues or len(set(queues)) != len(queues) or not set(queues) <= set(configured):
        parser.error("--devices must select distinct configured queues")
    mapping = {}
    for entry in args.device_map:
        queue, separator, ordinal = entry.partition("=")
        if not separator or queue not in queues or queue in mapping:
            parser.error("--device-map requires one QUEUE=ORDINAL entry per selected queue")
        mapping[queue] = ordinal
    mapping = {queue: mapping.get(queue, queue) for queue in queues}
    if any(not ordinal.isdecimal() or str(int(ordinal)) != ordinal for ordinal in mapping.values()):
        parser.error("visible ordinals must be canonical nonnegative integers")
    if len(set(mapping.values())) != len(mapping) or not set(args.rebind_device) <= set(queues):
        parser.error("each queue needs a distinct ordinal; rebind queues must be selected")
    per_device_memory = min(args.host_memory, args.host_memory_total // len(queues))
    if not per_device_memory:
        parser.error("aggregate host budget cannot cover selected devices")
    devices = {queue: Device(queue, ordinal) for queue, ordinal in mapping.items()}
    state_path = root / "supervisor.json"
    saved = json.loads(state_path.read_text()) if state_path.exists() else {}
    failures = {} if args.retry_failed else saved.get("failures", {})
    for device in devices.values():
        if failures.get(device.queue, 0) >= 3:
            device.done, device.state = True, "quarantined"
    network = network_log = None
    paused = stopping = False
    shutdown_deadline = None
    # File-only workers consume imported grants without spawning a network
    # child. The immutable configuration preserves this behavior on restart.
    network_enabled = config.get("transport", "https") == "https"
    initial_sync_checked = not network_enabled
    last_status = 0
    last_network_error = None
    status = read("status")
    sync_due = time.monotonic() + status["sync_due_in"] if network_enabled else None
    selector = selectors.DefaultSelector()
    self_tests = {}

    def control(signum, _frame):
        nonlocal paused, stopping, shutdown_deadline
        if signum in (signal.SIGINT, signal.SIGTERM):
            stopping = True
            if shutdown_deadline is None:
                shutdown_deadline = time.monotonic() + 30
            forward = signal.SIGTERM
        else:
            paused = signum == signal.SIGUSR1
            forward = signum
        for device in devices.values():
            if device.child and device.child.poll() is None:
                # Before the first event, SIGUSR1/2 may still have their default
                # disposition in the newly exec'd child. Apply pause after ready.
                if device.state != "pending" or stopping:
                    try:
                        device.child.send_signal(forward)
                    except ProcessLookupError:
                        pass
                if stopping and device.drain_at is None:
                    device.drain_at = time.monotonic()

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGUSR1, signal.SIGUSR2):
        signal.signal(sig, control)

    def events(device):
        # Bound both a single read and an unterminated line. A noisy or malformed
        # child cannot grow supervisor memory or starve healthy device polling.
        chunk = os.read(device.child.stdout.fileno(), 65536)
        if not chunk:
            selector.unregister(device.child.stdout)
            return
        device.buffer += chunk
        while b"\n" in device.buffer:
            line, device.buffer = device.buffer.split(b"\n", 1)
            if device.log_bytes + len(line) + 1 > 8 * 1024 * 1024:
                # Rotate on record boundaries and retain one prior file. Logs
                # are diagnostic; durable results live in SQLite/outbox receipts.
                device.log.close()
                path = root / ("execution-" + device.queue + ".log")
                os.replace(path, path.with_suffix(".log.1"))
                device.log = private_log(path)
                device.log_bytes = 0
            device.log.write(line + b"\n")
            device.log_bytes += len(line) + 1
            try:
                event = json.loads(line)
                if isinstance(event, dict):
                    device.event(event, time.monotonic())
                    if event.get("type") == "ready":
                        if paused:
                            device.child.send_signal(signal.SIGUSR1)
                        self_tests[device.queue] = event["self_test"]
                        private_write(root / "self-tests.json", json.dumps(self_tests) + "\n")
            except (ValueError, KeyError, TypeError):
                pass  # Diagnostic output is retained but cannot reset a watchdog.
        if len(device.buffer) > 65536:
            device.buffer = b""

    def start(device, now):
        words = command("run-device", "--device", device.ordinal, "--queue", device.queue,
                        "--backend", args.backend, "--once", "yes" if args.once else "no",
                        "--rebind", "yes" if device.queue in args.rebind_device else "no",
                        "--group-size", args.group_size,
                        "--batch-size", args.batch_size, "--giant-batch", args.giant_batch,
                        "--target-batch", args.target_batch, "--host-memory", per_device_memory)
        if args.kernel:
            words += ["--kernel", args.kernel]
        if args.batch_order:
            words += ["--batch-order", args.batch_order]
        if args.ordinal_order:
            words += ["--ordinal-order", args.ordinal_order]
        if args.ordinal_seed is not None:words += ["--ordinal-seed", args.ordinal_seed]
        if args.ordinal_window is not None:words += ["--ordinal-window", str(args.ordinal_window)]
        if args.tile_order:
            words += ["--tile-order", args.tile_order]
        if args.tile_seed is not None:words += ["--tile-seed", args.tile_seed]
        if args.tile_window is not None:words += ["--tile-window", str(args.tile_window)]
        if args.table:
            words += ["--table", str(args.table.resolve())]
        device.log = private_log(root / ("execution-" + device.queue + ".log"))
        device.log_bytes = os.fstat(device.log.fileno()).st_size
        device.child = subprocess.Popen(words, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(device.child.stdout.fileno(), False)
        selector.register(device.child.stdout, selectors.EVENT_READ, device)
        device.buffer, device.sequence = b"", 0
        device.last_progress, device.state, device.ready = now, "pending", False
        device.terminal_at = None
        device.drain_at, device.kill_sent = None, False

    try:
        while True:
            now = time.monotonic()
            for key, _ in selector.select(.05):
                events(key.data)
            for device in devices.values():
                child = device.child
                if child and child.poll() is not None:
                    # Drain events still buffered in the pipe before classifying
                    # exit. The old PID has been reaped before any replacement.
                    while child.stdout in [key.fileobj for key in selector.get_map().values()]:
                        events(device)
                    child.stdout.close()
                    device.log.close()
                    device.child = device.log = None
                    if (child.returncode or not device.ready) and not stopping:
                        failures[device.queue] = min(3, failures.get(device.queue, 0) + 1)
                        device.retry_at = now + min(30, 2 ** min(failures[device.queue], 5))
                    else:
                        device.done, device.state = True, "stopped"
                    if failures.get(device.queue, 0) >= 3:
                        device.done, device.state = True, "quarantined"
                if device.child:
                    if device.stalled(now, args.stall_seconds) and device.drain_at is None:
                        failures[device.queue] = 3
                        device.error = "execution stalled; bounded drain started"
                        device.child.terminate()
                        device.drain_at = now
                    if device.drain_at is not None and now - device.drain_at >= 30 and not device.kill_sent:
                        device.child.kill()
                        device.kill_sent, device.done = True, True
                        device.state = "quarantined"
                        failures[device.queue] = 3
            if network and network.poll() is not None:
                network_log.close()
                last_network_error = None if network.returncode == 0 else (root / "sync.log").read_text()[-2048:]
                network = network_log = None
                status = read("status")
                sync_due = now + status["sync_due_in"]
                initial_sync_checked = True
            if stopping:
                if not any(device.child for device in devices.values()) or now >= shutdown_deadline:
                    break
            else:
                if network_enabled and network is None and (now >= sync_due or not initial_sync_checked):
                    network_log = private_log(root / "sync.log")
                    network = subprocess.Popen(command("scheduled-sync"), stdout=network_log, stderr=subprocess.STDOUT)
                if initial_sync_checked and not paused:
                    for device in devices.values():
                        if not device.child and not device.done and now >= device.retry_at:
                            start(device, now)
                if args.once and initial_sync_checked and network is None and all(device.done for device in devices.values()):
                    break
            if now - last_status >= 1:
                status = read("status")
                private_write(state_path, json.dumps(dict(pid=os.getpid(), paused=paused, stopping=stopping,
                    devices={queue: device.snapshot() for queue, device in devices.items()},
                    sync_pid=network.pid if network else None, failures=failures,
                    host_memory_per_device=per_device_memory, host_memory_total=per_device_memory * len(devices),
                    last_network_error=last_network_error, worker=status)) + "\n")
                last_status = now
    finally:
        # Signal every child first, then share one drain deadline. One hung GPU
        # cannot multiply shutdown time by the fleet size. Kernel-held device and
        # block locks still prevent a new owner if an uninterruptible PID survives.
        children = [device.child for device in devices.values() if device.child] + ([network] if network else [])
        deadline = shutdown_deadline
        if children and all(device.child is None or device.kill_sent for device in devices.values()) and network is None:
            deadline = time.monotonic() # the execution watchdog already spent the drain budget
        survivors = stop_children(children, deadline)
        for device in devices.values():
            if device.child in survivors:
                failures[device.queue] = 3
                device.state, device.error = "quarantined", "owner did not exit; OS locks retained"
        selector.close()
        for device in devices.values():
            if device.log:
                device.log.close()
        if network_log:
            network_log.close()
        private_write(state_path, json.dumps(dict(pid=None, paused=paused, stopped=True, failures=failures,
            devices={queue: device.snapshot() for queue, device in devices.items()},
            last_network_error=last_network_error, worker=read("status"))) + "\n")
        os.close(lock)
    if args.once and last_network_error:
        raise RuntimeError(last_network_error)
    if any(failures.get(queue, 0) >= 3 for queue in devices):
        raise RuntimeError("device quarantined; inspect execution-QUEUE.log and explicitly retry after repair")


if __name__ == "__main__":
    main()
