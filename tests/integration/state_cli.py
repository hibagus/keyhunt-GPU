#!/usr/bin/env python3
"""Exercise public local-state commands with independent Python/SQLite checks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True, type=Path)
parser.add_argument("--report", required=True, type=Path)
args = parser.parse_args()
binary = args.binary.resolve()
checks = 0
with tempfile.TemporaryDirectory(prefix="keyhunt-c12-cli-") as temporary:
    root = Path(temporary)
    state = root / "state"

    def run(action, *options, directory=state, ok=True):
        global checks
        command = [str(binary), "state", action, "--state-dir", str(directory), *map(str, options)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        assert (result.returncode == 0) == ok, (command, result.returncode, result.stdout, result.stderr)
        checks += 1
        return json.loads(result.stdout) if ok else result.stderr

    assert run("init")["state_directory"] == str(state)
    assert state.stat().st_mode & 0o777 == 0o700
    assert (state / "progress.sqlite").stat().st_mode & 0o777 == 0o600
    run("init", "--bogus", "yes", ok=False)
    run("init", "--state-dir", root / "duplicate", ok=False)
    run("record-coverage", ok=False)
    run("init", directory="relative", ok=False)
    run("init", directory=Path(__file__).resolve().parents[2] / "bad-runtime-state", ok=False)

    project = run("project-create", "--name", 'Synthetic "C12"')['project']
    second_project = run("project-create", "--name", "Other project")["project"]
    manifest = ["--mode", "xpoint", "--range", "1:101", "--block-width", "10",
                "--target-digest", "11" * 32, "--algorithm-digest", "22" * 32]
    job = run("job-create", "--project", project, *manifest, "--seed", "33" * 32)["job"]
    # Independently encode the immutable manifest, without using library helpers.
    encoded = b"khjob\x01\x01" + b"".join(n.to_bytes(32, "big") for n in (1, 257, 16))
    encoded += bytes.fromhex("11" * 32 + "22" * 32)
    assert job == hashlib.sha256(encoded).hexdigest()
    assert run("job-create", "--project", project, *manifest)["job"] == job
    scope = ["--project", project, "--job", job]
    foreign = ["--project", second_project, "--job", job]
    run("inspect", *foreign, ok=False)
    assert run("job-create", "--project", second_project, *manifest)["job"] == job
    assert int(run("inspect", *foreign)["unexplored"], 16) == 16

    claims = run("claim", *scope, "--owner", "worker", "--request", "sequential", "--count", 3)
    assert [int(g["block"], 16) for g in claims["assignments"]] == [0, 1, 2]
    assert run("claim", *scope, "--owner", "worker", "--request", "sequential", "--count", 3) == claims
    run("claim", *scope, "--owner", "worker", "--request", "sequential", "--count", 4, ok=False)
    original = claims["assignments"][0]
    run("recover", *scope, "--block", "0", "--owner", "replacement", "--request", "recover", ok=False)
    replacement = run("recover", *scope, "--block", "0", "--owner", "replacement",
                      "--request", "recover", "--previous-stopped", "yes")
    assert int(replacement["generation"]) > int(original["generation"])
    run("renew", "--grant", original["grant"], "--request", "stale", ok=False)
    renewal = run("renew", "--grant", replacement["grant"], "--request", "renew")
    assert renewal == run("renew", "--grant", replacement["grant"], "--request", "renew")
    assert renewal["generation"] == replacement["generation"]
    run("return", "--grant", renewal["grant"], "--request", "return")
    run("return", "--grant", renewal["grant"], "--request", "return")
    assert run("block", *scope, "--block", "0")["state"] == "unexplored"
    reclaimed = run("claim", *scope, "--owner", "worker", "--request", "manual",
                    "--policy", "manual", "--block", "0")["assignments"][0]
    assert int(reclaimed["generation"]) > int(replacement["generation"])
    run("claim", *scope, "--owner", "worker", "--request", "occupied",
        "--policy", "manual", "--block", "0", ok=False)
    run("claim", *scope, "--owner", "worker", "--request", "outside",
        "--policy", "manual", "--block", "10", ok=False)
    run("claim", *scope, "--owner", "worker", "--request", "bad", "--window", "4", ok=False)
    run("claim", *scope, "--owner", "worker", "--request", "bad", "--count", "257", ok=False)

    # A failed stdout occurs after commit. Its receipt must survive process exit.
    failed_command = [str(binary), "state", "claim", "--state-dir", str(state), *scope,
                      "--owner", "worker", "--request", "lost-output", "--count", "2"]
    with open("/dev/full", "w") as output:
        failed = subprocess.run(failed_command, stdout=output, stderr=subprocess.PIPE, timeout=30)
    assert failed.returncode == 2 and b"output failed" in failed.stderr
    lost = run("claim", *scope, "--owner", "worker", "--request", "lost-output", "--count", "2")
    assert [int(g["block"], 16) for g in lost["assignments"]] == [3, 4]

    window = run("claim", *scope, "--owner", "worker", "--request", "window",
                 "--policy", "random-window", "--window", "4", "--count", "256")["assignments"]
    assert window and len({int(g["block"], 16) // 4 for g in window}) == 1
    reserved = {0, 1, 2, 3, 4} | {int(g["block"], 16) for g in window}
    random = run("claim", *scope, "--owner", "worker", "--request", "random",
                 "--policy", "random", "--count", "256")["assignments"]
    assert {int(g["block"], 16) for g in random} == set(range(16)) - reserved
    assert run("claim", *scope, "--owner", "worker", "--request", "empty")["assignments"] == []
    assert run("inspect", *scope)["assignments"] == 16
    assert run("inspect", *foreign)["assignments"] == 0

    # Separate CLI processes racing on one request return one committed response.
    def concurrent(_):
        return run("claim", *foreign, "--owner", "shared", "--request", "same", "--count", "8")
    with ThreadPoolExecutor(max_workers=4) as pool:
        responses = list(pool.map(concurrent, range(4)))
    assert all(response == responses[0] for response in responses)
    assert run("inspect", *foreign)["assignments"] == 8

    # All C++ writers have exited before this independent SQLite reader starts.
    with sqlite3.connect(state / "progress.sqlite") as db:
        db.execute("PRAGMA foreign_keys=ON")
        assert db.execute("SELECT typeof(block),length(block) FROM assignments").fetchall() == [("blob", 32)] * 24
        try:
            db.execute("INSERT INTO coverage VALUES(?,?,?,?,?)",
                       (second_project, bytes.fromhex(job), (15).to_bytes(32, "big"),
                        (1).to_bytes(32, "big"), (2).to_bytes(32, "big")))
        except sqlite3.IntegrityError:
            db.rollback()
        else:
            raise AssertionError("cross-project coverage was accepted")
    db.close()

    run("check")
    before = run("inspect", *scope)
    run("compact")
    after = run("inspect", *scope)
    for field in ("blocks", "unexplored", "finished", "assignments", "tree_nodes", "requests", "events"):
        assert before[field] == after[field]

    backup, restored = root / "backup", root / "restored"
    run("backup", "--destination", backup)
    run("backup", "--destination", backup, ok=False)
    run("restore", "--source", backup, directory=restored)
    for directory in (backup, restored):
        assert run("inspect", *scope, directory=directory)["quarantined"]
        run("check", directory=directory)
        run("claim", *scope, "--owner", "worker", "--request", "unsafe", directory=directory, ok=False)
        run("renew", "--grant", reclaimed["grant"], "--request", "unsafe", directory=directory, ok=False)
    run("restore", "--source", root / "missing", directory=root / "bad-restore", ok=False)
    assert not (root / "missing").exists()

    # High-bit IDs survive JSON, arithmetic and SQLite without narrowing.
    huge_manifest = ["--mode", "bsgs", "--range", "1:fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141",
                     "--block-width", "1", "--target-digest", "44" * 32, "--algorithm-digest", "55" * 32]
    huge_job = run("job-create", "--project", project, *huge_manifest)["job"]
    huge_scope = ["--project", project, "--job", huge_job]
    assert run("inspect", *huge_scope)["tree_nodes"] == 0
    wide_id = 2**240 + 123
    wide = run("claim", *huge_scope, "--owner", "wide", "--request", "wide",
               "--policy", "manual", "--block", hex(wide_id))["assignments"][0]
    assert int(wide["block"], 16) == wide_id and int(wide["begin"], 16) == wide_id + 1
    assert run("inspect", *huge_scope)["tree_nodes"] <= 257
    run("check")

    # Environment precedence must never touch the real user's state directory.
    for variable, suffix in (("KEYHUNT_STATE_DIR", ""), ("XDG_STATE_HOME", "keyhunt"), ("HOME", ".local/state/keyhunt")):
        env = dict(os.environ)
        env.pop("KEYHUNT_STATE_DIR", None)
        env.pop("XDG_STATE_HOME", None)
        env[variable] = str(root / variable)
        result = subprocess.run([str(binary), "state", "init"], env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["state_directory"] == str(root / variable / suffix)
        checks += 1

report = {"passed": True, "command_checks": checks, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
          "cases": ["strict CLI", "independent manifest hash", "project isolation", "idempotency",
                    "lost stdout", "live transfer confirmation", "generation fencing", "all policies",
                    "concurrent CLI retries", "composite foreign keys", "compaction", "sealed backup/restore",
                    "wide sparse IDs", "environment precedence"]}
args.report.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
