#!/usr/bin/env python3
"""Two isolated native worker journals, real mTLS, and optional HIP supervision."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from coordinator_local import Environment, HOST, REPO

GX = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
GY = "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator", required=True)
    parser.add_argument("--worker", required=True)
    parser.add_argument("--keyhunt", required=True)
    parser.add_argument("--apache-root", default="/")
    parser.add_argument("--hardware", "--hip", dest="hardware", action="store_true")
    parser.add_argument("--backend", choices=("hip", "cuda"), default="hip")
    args = parser.parse_args()
    worker, keyhunt = str(Path(args.worker).resolve()), str(Path(args.keyhunt).resolve())
    with tempfile.TemporaryDirectory(prefix="kh-workers-") as directory:
        root = Path(directory)
        env = Environment(root / "server", args.coordinator, args.apache_root)
        try:
            env.initialize()
            env.start()
            alice = env.admin("bootstrap", name="alice", certificate=(env.directory / "alice.pem").read_text())
            bob = env.admin("client-add", name="bob", certificate=(env.directory / "bob.pem").read_text())
            projects = [env.admin("project-create", name=name, owner=owner["client"])["project"]
                        for name, owner in (("first", alice), ("second", bob))]
            env.admin("membership-set", project=projects[0], client=bob["client"], role="worker")

            def api(name, method, path, body=None):
                with env.client(name) as client:
                    client.request(method, path, None if body is None else json.dumps(body), {"Content-Type": "application/json"})
                    response = client.getresponse()
                    payload = response.read()
                    assert response.status == 200, (response.status, payload)
                    return json.loads(payload)["value"]

            def invoke(state, action, *extra, ok=True):
                result = subprocess.run([worker, action, "--state-dir", str(state), *map(str, extra)],
                                        text=True, capture_output=True, timeout=60)
                assert (result.returncode == 0) == ok, (action, result.stdout, result.stderr)
                return json.loads(result.stdout) if ok else result

            def configure(state, name, project, job, **overrides):
                config = dict(endpoint="https://" + env.authority, ca=str(env.directory / "server-ca.pem"),
                              certificate=str(env.directory / (name + ".pem")), key=str(env.directory / (name + ".key")),
                              resolve=f"{HOST}:{env.port}:127.0.0.1",
                              jobs=[dict(project=project, job=job, devices=["0"], spares=0, policy="sequential")])
                config.update(overrides)
                path = root / (state.name + ".json")
                path.write_text(json.dumps(config))
                os.chmod(path, 0o600)
                invoke(state, "configure", "--config", path)
                return config

            def supervise(state, table=None):
                words = [sys.executable, str(REPO / "tools/coordinator_worker.py"), "--state-dir", str(state),
                         "--worker", worker, "--keyhunt", keyhunt, "--backend", args.backend, "--once"]
                if table:
                    words += ["--table", str(table)]
                result = subprocess.run(words, capture_output=True, text=True, timeout=180)
                assert result.returncode == 0, (result.stdout, result.stderr, (state / "execution.log").read_text() if (state / "execution.log").exists() else "")
                assert json.loads((state / "self-tests.json").read_text())["0"]["passed"]

            body = dict(mode="xpoint", begin=f"0x{1:064x}", end_exclusive=f"0x{4097:064x}",
                        block_width=f"0x{1024:064x}", configuration=(b"khsearch\x01\x01" + bytes(40)).hex(), targets=GX)
            job = api("alice", "POST", f"/api/v1/projects/{projects[0]}/jobs", body)
            other = api("bob", "POST", f"/api/v1/projects/{projects[1]}/jobs", body)
            assert job["job"] == other["job"]
            states = [root / "alice-worker", root / "bob-worker"]
            for state, name in zip(states, ("alice", "bob")):
                configure(state, name, projects[0], job["job"])
                assert invoke(state, "sync")["sent"]
                assert not invoke(state, "scheduled-sync")["sent"]
            grants = [invoke(state, "next", "--device", "0")["grant"] for state in states]
            assert grants[0]["block"] != grants[1]["block"]
            # The same certificate with a wrong server CA or hostname cannot
            # establish the transport. No insecure option exists in the client.
            bad = root / "wrong-ca-worker"
            configure(bad, "alice", projects[0], job["job"], ca=str(env.directory / "wrong-ca.pem"))
            assert "HTTPS request failed" in invoke(bad, "sync", ok=False).stderr
            bad_host = root / "wrong-host-worker"
            configure(bad_host, "alice", projects[0], job["job"], endpoint=f"https://wrong.invalid:{env.port}",
                      resolve=f"wrong.invalid:{env.port}:127.0.0.1")
            assert "HTTPS request failed" in invoke(bad_host, "sync", ok=False).stderr
            request = root / "api.json"
            request.write_text(json.dumps(dict(method="GET", path=f"/api/v1/projects/{projects[1]}/jobs/{job['job']}/status")))
            assert '"status":404' in invoke(states[0], "api", "--request", request, ok=False).stderr
            if args.hardware:
                for state in states:
                    supervise(state)
                    status = invoke(state, "status")
                    assert status["outbox_bytes"] > 0
                    assert status["queues"][0]["activity"] == "local-complete-awaiting-sync"
                    assert not invoke(state, "scheduled-sync")["sent"]
                path = f"/api/v1/projects/{projects[0]}/jobs/{job['job']}"
                assert api("alice", "GET", path + "/status")["finished"] == f"0x{0:064x}"
                for state in states:
                    assert invoke(state, "sync")["outbox_bytes"] == 0
                assert api("alice", "GET", path + "/status")["finished"] == f"0x{2:064x}"
                assert len(api("alice", "GET", path + "/results")) == 1

                table = root / "babies.khb"
                result = subprocess.run([keyhunt, "bsgs-table", "build", "--m", "17", "--output", str(table)], capture_output=True, text=True, check=True, timeout=60)
                metadata = json.loads(result.stdout)
                checksum = metadata["checksum"]
                config = b"khsearch\x01\x02" + (17).to_bytes(8, "big") + bytes.fromhex(checksum)
                body.update(mode="bsgs", end_exclusive=f"0x{500:064x}", block_width=f"0x{499:064x}", configuration=config.hex(), targets="04" + GX + GY)
                bjob = api("bob", "POST", f"/api/v1/projects/{projects[1]}/jobs", body)
                state = root / "bsgs-worker"
                configure(state, "bob", projects[1], bjob["job"])
                # First scheduled sync is driven by the supervisor itself.
                supervise(state, table)
                assert invoke(state, "status")["outbox_bytes"] > 0
                invoke(state, "sync")
                bpath = f"/api/v1/projects/{projects[1]}/jobs/{bjob['job']}"
                assert api("bob", "GET", bpath + "/status")["finished"] == f"0x{1:064x}"
                assert len(api("bob", "GET", bpath + "/results")) == 1
                # Persistent device ownership must retain its table upload over
                # multiple short grants. Both leases stay pending until a later
                # explicit machine sync, independent of block completion.
                body.update(end_exclusive=f"0x{1001:064x}", block_width=f"0x{500:064x}")
                persistent_job = api("bob", "POST", f"/api/v1/projects/{projects[1]}/jobs", body)
                state = root / "persistent-worker"
                configure(state, "bob", projects[1], persistent_job["job"], jobs=[dict(
                    project=projects[1], job=persistent_job["job"], devices=["0"], spares=1, policy="sequential")])
                invoke(state, "sync")
                result = subprocess.run([worker, "run-device", "--state-dir", str(state), "--device", "0",
                    "--backend", args.backend, "--table", str(table), "--once", "yes"], capture_output=True, text=True, timeout=120)
                assert result.returncode == 0, (result.stdout, result.stderr)
                events = [json.loads(line) for line in result.stdout.splitlines()]
                assert events[-1]["completed"] == 2 and events[-1]["executor_setups"] == 1, events
                assert len([row for row in events if row["type"] == "ready"]) == 1
                assert invoke(state, "status")["outbox_bytes"] > 0
                assert not invoke(state, "scheduled-sync")["sent"]
                invoke(state, "sync")
                ppath = f"/api/v1/projects/{projects[1]}/jobs/{persistent_job['job']}"
                assert api("bob", "GET", ppath + "/status")["finished"] == f"0x{2:064x}"
                assert len(api("bob", "GET", ppath + "/results")) == 1
                # HASH160 uses the same authenticated queue and persistent owner.
                # Two serialization relations at scalar 1 must survive both the
                # local journal and a later server acknowledgment without merging.
                body.update(mode="hash160",end_exclusive=f"0x{1025:064x}",block_width=f"0x{512:064x}",
                    configuration=(b"khsearch\x01\x03"+bytes(40)).hex(),
                    targets="01751e76e8199196d454941c45d1b3a323f1433bd6"+"0291b24bf9f5288532960ac687abb035127b1d28a5")
                hjob=api("bob","POST",f"/api/v1/projects/{projects[1]}/jobs",body)
                state=root/"hash160-worker"
                configure(state,"bob",projects[1],hjob["job"],jobs=[dict(
                    project=projects[1],job=hjob["job"],devices=["0"],spares=1,policy="sequential")])
                invoke(state,"sync");supervise(state)
                events=[json.loads(line) for line in (state/"execution-0.log").read_text().splitlines()]
                finished=[row for row in events if row.get("type")=="grant-finish"]
                assert len(finished)==2 and all(row["mode"]=="hash160" and row["executor_setups"]==1 for row in finished)
                assert finished[0]["cold"] and not finished[1]["cold"]
                assert invoke(state,"status")["outbox_bytes"]>0 and not invoke(state,"scheduled-sync")["sent"]
                invoke(state,"sync")
                hpath=f"/api/v1/projects/{projects[1]}/jobs/{hjob['job']}"
                assert api("bob","GET",hpath+"/status")["finished"]==f"0x{2:064x}"
                results=api("bob","GET",hpath+"/results")
                assert len(results)==2 and all(int(row["scalar"],16)==1 for row in results)
                # Ethereum must retain its Keccak mode and executor across grants.
                body.update(mode="ethereum",end_exclusive=f"0x{1025:064x}",block_width=f"0x{512:064x}",
                    configuration=(b"khsearch\x01\x04"+bytes(40)).hex(),
                    targets="7e5f4552091a69125d5dfcb7b8c2659029395bdf")
                ejob=api("bob","POST",f"/api/v1/projects/{projects[1]}/jobs",body)
                state=root/"ethereum-worker"
                configure(state,"bob",projects[1],ejob["job"],jobs=[dict(
                    project=projects[1],job=ejob["job"],devices=["0"],spares=1,policy="sequential")])
                invoke(state,"sync");supervise(state)
                events=[json.loads(line) for line in (state/"execution-0.log").read_text().splitlines()]
                finished=[row for row in events if row.get("type")=="grant-finish"]
                assert len(finished)==2 and all(row["mode"]=="ethereum" and row["executor_setups"]==1 for row in finished)
                assert finished[0]["cold"] and not finished[1]["cold"]
                assert invoke(state,"status")["outbox_bytes"]>0 and not invoke(state,"scheduled-sync")["sent"]
                invoke(state,"sync")
                epath=f"/api/v1/projects/{projects[1]}/jobs/{ejob['job']}"
                assert api("bob","GET",epath+"/status")["finished"]==f"0x{2:064x}"
                results=api("bob","GET",epath+"/results")
                assert len(results)==1 and all(int(row["scalar"],16)==1 for row in results)
            env.admin("check")
        except BaseException:
            for file in env.directory.glob("*.log"):
                print(file.name, file.read_text()[-5000:], file=sys.stderr)
            raise
        finally:
            env.stop()
    print("Two native HTTPS workers and project isolation passed" + ("; supervised GPU xpoint/BSGS/HASH160/Ethereum passed" if args.hardware else ""))


if __name__ == "__main__":
    main()
