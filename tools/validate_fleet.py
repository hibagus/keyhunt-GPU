#!/usr/bin/env python3
"""Validate synthetic multi-GPU execution against isolated localhost mTLS.

All private credentials, leases, logs and tables live in an external temporary
directory. The exported report contains hardware/build metadata and measured
synthetic results, never private credentials. No partition changes or GPU resets.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from coordinator_local import Environment, HOST, REPO

GX = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
GY = "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8"
P = 2 ** 256 - 2 ** 32 - 977


def invoke(words, timeout=60, ok=True):
    result = subprocess.run(list(map(str, words)), capture_output=True, text=True, timeout=timeout)
    if (result.returncode == 0) != ok:
        raise AssertionError((words, result.returncode, result.stdout[-3000:], result.stderr[-3000:]))
    return result


def rows(path):
    result = []
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                row = json.loads(line)
                if isinstance(row, dict):
                    result.append(row)
            except ValueError:
                pass
    return result


def until(predicate, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError("fleet condition timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--apache-root", required=True)
    parser.add_argument("--backend", choices=("hip", "cuda"), default="hip")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--counts", default="1,2,4,8")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--block-bits", type=int, default=32)
    parser.add_argument("--lifecycle", action="store_true", help="also exercise real long pauses, stopped owners and memory pressure")
    parser.add_argument("--lifecycle-only", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    worker, binary, coordinator = (build / name for name in ("keyhunt-worker", "keyhunt", "keyhunt-coordinator"))
    counts = [int(value) for value in args.counts.split(",")]
    inventory = json.loads(invoke([binary, "devices", "--backend", args.backend]).stdout)
    if max(counts) > len(inventory["devices"]) or ((args.lifecycle or args.lifecycle_only) and len(inventory["devices"]) < 2):
        print("requested fleet is not visible", file=sys.stderr)
        return 77
    if args.repeat < 1 or not 4 <= args.block_bits <= 48:
        parser.error("repeat must be positive and block-bits must be 4..48")
    report = dict(inventory=inventory, runs=[], lifecycle={}, passed=False,
        source_commit=invoke(["git", "-C", REPO, "rev-parse", "HEAD"]).stdout.strip(),
        source_diff_sha256=hashlib.sha256(invoke(["git", "-C", REPO, "diff", "HEAD"]).stdout.encode()).hexdigest(),
        binaries={path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (worker, binary, coordinator)},
        source_sha256={str(path.relative_to(REPO)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [REPO / "tools/validate_fleet.py", REPO / "tools/coordinator_worker.py",
                REPO / "src/coordinator/device_worker.cpp", REPO / "src/coordinator/worker.cpp",
                REPO / "src/storage/checkpoint.cpp", REPO / "src/coordinator/self_test.cpp",
                REPO / "src/backend/hip/discovery.hip", REPO / "src/backend/cuda/discovery.cu"]},
        other_load="No additional GPU workload launched by this validation; unrelated host load is uncontrolled.")

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    with tempfile.TemporaryDirectory(prefix="kh-c20-fleet-") as directory:
        root = Path(directory)
        env = Environment(root / "server", coordinator, args.apache_root)
        processes = []
        try:
            env.initialize()
            env.start()
            owner = env.admin("bootstrap", name="synthetic", certificate=(env.directory / "alice.pem").read_text())
            table = root / "table.khb"
            table_meta = json.loads(invoke([binary, "bsgs-table", "build", "--m", "257", "--output", table]).stdout)
            bindings = dict(xpoint=(b"khsearch\x01\x01" + bytes(40)).hex(),
                bsgs=(b"khsearch\x01\x02" + (257).to_bytes(8, "big") + bytes.fromhex(table_meta["checksum"])).hex())
            targets = dict(xpoint=GX, bsgs="04" + GX + GY + "04" + GX + f"{P - int(GY, 16):064x}")

            next_http = 0.0
            def pace_http():
                nonlocal next_http
                # Validation makes several explicit inspection/upload requests
                # per tiny job. Respect the production 120/minute client budget.
                time.sleep(max(0, next_http - time.monotonic()))
                next_http = time.monotonic() + .6

            def api(method, path, body=None):
                pace_http()
                with env.client("alice") as client:
                    client.request(method, path, None if body is None else json.dumps(body), {"Content-Type": "application/json"})
                    response = client.getresponse()
                    payload = response.read()
                    assert response.status == 200, (response.status, payload)
                    return json.loads(payload)["value"]

            def native(state, action, *extra):
                if action == "sync":
                    pace_http()
                return json.loads(invoke([worker, action, "--state-dir", state, *extra]).stdout)

            def create(label, mode, queues, width, spares=1):
                project = env.admin("project-create", name=label, owner=owner["client"])["project"]
                end = 1 + width * len(queues) * (1 + spares) - 7
                body = dict(mode=mode, begin=f"0x{1:064x}", end_exclusive=f"0x{end:064x}",
                    block_width=f"0x{width:064x}", configuration=bindings[mode], targets=targets[mode])
                job = api("POST", f"/api/v1/projects/{project}/jobs", body)
                return dict(project=project, job=job["job"], devices=queues, spares=spares, policy="sequential"), end

            def configure(label, jobs):
                state = root / label
                config = dict(endpoint="https://" + env.authority, ca=str(env.directory / "server-ca.pem"),
                    certificate=str(env.directory / "alice.pem"), key=str(env.directory / "alice.key"),
                    resolve=f"{HOST}:{env.port}:127.0.0.1", jobs=jobs)
                file = root / (label + ".json")
                file.write_text(json.dumps(config));file.chmod(0o600)
                native(state, "configure", "--config", file)
                native(state, "sync")
                return state

            def words(state, *extra):
                return [sys.executable, REPO / "tools/coordinator_worker.py", "--state-dir", state,
                    "--worker", worker, "--keyhunt", binary, "--backend", args.backend, "--table", table, *extra]

            def start(state, *extra):
                log = open(root / (state.name + ".supervisor.log"), "w")
                process = subprocess.Popen(list(map(str, words(state, *extra))), stdout=log, stderr=subprocess.STDOUT)
                processes.append((process, log))
                return process

            def stop(process, expected=0):
                if process.poll() is None:
                    process.terminate()
                assert process.wait(timeout=40) == expected

            def control(state, queue, action="status"):
                return json.loads(invoke([binary, "checkpoint", action, "--state-dir", state, "--slot", queue]).stdout)

            def event(state, queue, kind):
                return [row for row in rows(state / f"execution-{queue}.log") if row.get("type") == kind]

            if not args.lifecycle_only:
                for mode in ("xpoint", "bsgs"):
                    for count in counts:
                        for repetition in range(args.repeat + 1):
                            label = f"{mode}-{count}-{repetition}"
                            queues = list(map(str, range(count)))
                            job, end = create(label, mode, queues, 1 << args.block_bits)
                            state = configure(label, [job])
                            started = time.monotonic_ns()
                            result = invoke(words(state, "--once"), timeout=300)
                            wall_ns = time.monotonic_ns() - started
                            status = native(state, "status")
                            assert status["outbox_bytes"] > 0
                            assert all(row["activity"] == "local-complete-awaiting-sync" for row in status["queues"]), status
                            self_tests = json.loads((state / "self-tests.json").read_text())
                            assert len(self_tests) == count and all(test["passed"] for test in self_tests.values())
                            grants = []
                            for queue in queues:
                                exits = event(state, queue, "exit")
                                assert exits[-1]["executor_setups"] <= 1, exits
                                grants.extend(dict(row, queue=queue, uuid=self_tests[queue]["uuid"])
                                    for row in event(state, queue, "grant-finish"))
                            intervals = sorted((int(row["grant"]["begin"], 16), int(row["grant"]["end_exclusive"], 16)) for row in grants)
                            assert len(intervals) == count * 2 and intervals[0][0] == 1 and intervals[-1][1] == end
                            assert all(left[1] == right[0] for left, right in zip(intervals, intervals[1:])), intervals
                            assert sum(int(row["computed_scalars"], 16) for row in grants) == end - 1
                            invoke([binary, "state", "check", "--state-dir", state])
                            path = f"/api/v1/projects/{job['project']}/jobs/{job['job']}"
                            assert int(api("GET", path + "/status")["finished"], 16) == 0, "hidden completion upload"
                            native(state, "sync")
                            assert int(api("GET", path + "/status")["finished"], 16) == count * 2
                            matches = api("GET", path + "/results")
                            assert len(matches) == 1 and int(matches[0]["scalar"], 16) == 1, matches
                            report["runs"].append(dict(mode=mode, device_count=count, repetition=repetition,
                                measured=repetition > 0, validated=True, wall_ns=wall_ns, scalar_count=str(end - 1),
                                configuration=bindings[mode], targets=targets[mode], grants=grants))
                            save()
                            print(f"{label}: exact {count * 2} blocks, {wall_ns / 1e9:.3f}s", flush=True)

            if args.lifecycle or args.lifecycle_only:
                job, _ = create("lifecycle", "xpoint", ["0", "1"], 1 << 45)
                state = configure("lifecycle-worker", [job])
                process = start(state, "--stall-seconds", "60")
                until(lambda: all(event(state, queue, "progress") for queue in ("0", "1")))
                initial = {queue: control(state, queue)["pid"] for queue in ("0", "1")}
                path = f"/api/v1/projects/{job['project']}/jobs/{job['job']}"
                # Authentication and worker request replay remain real here.
                for cycle in range(3):
                    api("POST", path + "/pause", dict(paused=True));native(state, "sync")
                    until(lambda: all(len(event(state, queue, "blocked")) > cycle for queue in ("0", "1")))
                    api("POST", path + "/pause", dict(paused=False));native(state, "sync")
                    until(lambda: all(control(state, queue)["state"] == "running" for queue in ("0", "1")))
                assert all(control(state, queue)["pid"] == initial[queue] for queue in initial)
                report["lifecycle"]["server_pause_cycles_same_process"] = 3
                save();print("lifecycle: three authenticated pause cycles kept both PIDs", flush=True)
                control(state, "0", "pause")
                until(lambda: control(state, "0")["durably_paused"])
                healthy_before = event(state, "1", "progress")[-1]["sequence"]
                started = time.monotonic()
                time.sleep(65)  # Deliberately exceeds the supported 60-second watchdog.
                assert control(state, "0")["durably_paused"] and control(state, "0")["pid"] == initial["0"]
                assert event(state, "1", "progress")[-1]["sequence"] > healthy_before
                saved = json.loads((state / "supervisor.json").read_text())
                assert not saved["failures"], saved
                report["lifecycle"]["socket_pause_seconds"] = time.monotonic() - started
                save();print("lifecycle: socket pause exceeded watchdog; healthy peer progressed", flush=True)
                control(state, "0", "resume")
                until(lambda: control(state, "0")["state"] == "running")
                # A stopped host owner models an unresponsive submission without
                # resetting a GPU or depending on an actual driver fault.
                os.kill(initial["0"], signal.SIGSTOP)
                healthy_before = event(state, "1", "progress")[-1]["sequence"]
                assert native(state, "sync")["sent"]
                until(lambda: json.loads((state / "supervisor.json").read_text())["devices"]["0"]["state"] == "quarantined", timeout=100)
                assert event(state, "1", "progress")[-1]["sequence"] > healthy_before
                report["lifecycle"]["stopped_owner_quarantined_healthy_progress_and_sync"] = True
                save();print("lifecycle: unresponsive owner quarantined; peer and sync continued", flush=True)
                stop(process, expected=1)
                invoke([binary, "state", "check", "--state-dir", state])
                # Select only the healthy queue on restart; the quarantined slot
                # retains its block, and saved failure counts do not block peers.
                process = start(state, "--devices", "1")
                def restarted():
                    try:
                        return control(state, "1")["pid"] != initial["1"]
                    except AssertionError:
                        return False
                until(restarted)
                stop(process)
                report["lifecycle"]["changed_device_count_resume"] = True
                bjob, _ = create("memory-bsgs", "bsgs", ["0"], 1024)
                xjob, _ = create("memory-xpoint", "xpoint", ["1"], 1024)
                pressure = configure("memory-worker", [bjob, xjob])
                invoke(words(pressure, "--once", "--host-memory", "128"), timeout=45, ok=False)
                saved = json.loads((pressure / "supervisor.json").read_text())
                assert saved["failures"].get("0") == 3 and saved["failures"].get("1", 0) == 0, saved
                assert saved["devices"]["1"]["completed"] == 2, saved
                invoke([binary, "state", "check", "--state-dir", pressure])
                report["lifecycle"]["memory_pressure_isolated"] = True
                save()
            env.admin("check")
            report["passed"] = True
            save()
        except BaseException:
            save()
            for file in root.glob("*.supervisor.log"):
                print(file.name, file.read_text()[-4000:], file=sys.stderr)
            raise
        finally:
            for process, log in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=40)
                    except subprocess.TimeoutExpired:
                        process.kill();process.wait(timeout=10)
                log.close()
            env.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
