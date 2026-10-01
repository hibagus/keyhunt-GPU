#!/usr/bin/env python3
"""Pause real HIP work, snapshot it, then resume across visible device layouts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "oracle"))
from oracle_selftest import check_source, run as oracle_run
from model import N
L=int('5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72',16)
from hash160 import hash160,address
from ethereum import address as eth_address
from minikey import PUBLIC_KEYS,text as minikey_text,ordinal as minikey_ordinal,scalar as minikey_scalar

parser = argparse.ArgumentParser()
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--oracle", type=Path, required=True)
parser.add_argument("--report", type=Path, required=True)
parser.add_argument("--backend",choices=("hip","cuda"),default="hip")
parser.add_argument("--mode", choices=("xpoint","bsgs","hash160","ethereum","vanity","minikeys22","minikeys30"), action="append")
parser.add_argument("--stride", type=lambda value:int(value,16), default=1)
parser.add_argument("--order",choices=("forward","reverse"),default="forward")
parser.add_argument("--ordinal-order",choices=("forward","reverse"))
parser.add_argument("--tile-order",choices=("forward","reverse","both-ends","dance"))
parser.add_argument("--orbit",action="store_true")
parser.add_argument("--kernel", choices=("direct","stepped","glv"), help="first stage kernel; later stages switch to direct and stepped")
args = parser.parse_args()
if args.kernel and (not args.mode or any(mode not in ("xpoint","hash160","ethereum","vanity") for mode in args.mode)):
    parser.error("kernel switching requires explicit scalar modes")
if (args.orbit or args.stride!=1 or args.order=="reverse") and (not args.mode or any(mode not in ("xpoint","hash160","ethereum","vanity") for mode in args.mode)):
    parser.error("strides require explicit scalar modes")
if args.ordinal_order and (not args.mode or any(v not in ("minikeys22","minikeys30") for v in args.mode)):
    parser.error("ordinal-order requires explicit minikey modes")
if args.tile_order and args.mode!=["bsgs"]:
    parser.error("tile-order requires explicit BSGS mode")
binary = args.binary.resolve()
report = {"ordinal_order":args.ordinal_order,"tile_order":args.tile_order,"orbit":args.orbit,"oracle_commit": check_source(), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
          "cases": [], "kernels": [args.kernel,"direct","stepped"] if args.kernel else ["direct"]*3 if args.mode and all(v.startswith("minikeys") for v in args.mode) else ["stepped"]*3, "pause_latency_scope": "local socket request through durably-paused status, including admitted batch and up to 20 ms idle polling"}
def invoke(words, env=None, ok=True):
    result = subprocess.run([str(binary), *map(str, words)], capture_output=True, text=True, timeout=90, env=env)
    assert (result.returncode == 0) == ok, (words, result.stdout, result.stderr)
    return [json.loads(line) for line in result.stdout.splitlines()] if ok else result

inventory = invoke(["devices", "--backend", args.backend])[0]
report["inventory"] = inventory
count = len(inventory["devices"])
restricted = any(os.environ.get(k) for k in ("HIP_VISIBLE_DEVICES", "ROCR_VISIBLE_DEVICES", "CUDA_VISIBLE_DEVICES"))
layouts = [(None, 0, count)] * 3
if count >= 2 and not restricted:
    layouts = [("0,1", 1, 2), ("1", 0, 1), ("0,1", 0, 2)]
    if count >= 3:
        layouts[-1] = ("0,1,2", 2, 3)
report["changed_inventory_tested"] = layouts[0][2] != layouts[1][2]

with tempfile.TemporaryDirectory(prefix="kh-c14-hip-") as temporary:
    root = Path(temporary)
    table = root / "table.khb"
    invoke(["bsgs-table", "build", "--m", "17", "--output", table])
    candidate_count=1048576*(6 if args.orbit else 1)
    def seed(index):
        offset=(index-1)%1048576
        return 1+((1048575-offset) if args.order=="reverse" else offset)*args.stride
    def scalar(index):
        variant=(index-1)//1048576 if args.orbit else 0
        return seed(index)*pow(L,variant//2,N)*(-1 if variant%2 else 1)%N
    seeds = [scalar(v*1048576+i) for v in range(6 if args.orbit else 1) for i in (1,2,1048576)]+[1+((1<<80)-1)*args.stride]
    public = dict(zip(seeds, oracle_run(args.oracle, [f"pub {n:064x}" for n in seeds])))
    for mode in (args.mode or ("xpoint", "bsgs")):
        state = root / mode
        def local(family, action, *words, directory=None, ok=True, env=None):
            return invoke([family, action, "--state-dir", directory or state, *words], env=env, ok=ok)
        targets = root / (mode + ".txt")
        values = ({(n,tag):hash160(p,tag) for n,p in public.items() for tag in (1,2)} if mode=="hash160"
                  else {(n,tag):address(hash160(p,tag)) for n,p in public.items() for tag in (1,2)} if mode=="vanity"
                  else {n: p[2:66] if mode == "xpoint" else eth_address(p) if mode=="ethereum" else p for n, p in public.items()})
        length=int(mode[-2:]) if mode.startswith("minikeys") else None
        begin=1
        if length:
            key=PUBLIC_KEYS[0 if length==22 else 1];match_ordinal=minikey_ordinal(key)
            begin=match_ordinal-1048575 if args.ordinal_order=="reverse" else match_ordinal
            point=oracle_run(args.oracle,[f"pub {minikey_scalar(key):064x}"])[0]
            values={(match_ordinal,tag):hash160(point,tag) for tag in (1,2)}
        targets.write_text("\n".join(values.values()) + "\n")
        inputs = ["--targets", targets] + (["--table", table] if mode == "bsgs" else [])
        if length:inputs += ["--length",length,"--input-format","hash160"]
        project = local("state", "project-create", "--name", "C14 HIP pause")[0]["project"]
        job = local("checkpoint", "create", "--project", project, "--mode", "minikeys" if length else mode, "--range", f"{begin:x}:{begin+1048576*args.stride:x}",
                    "--block-width", f"{candidate_count:x}", *inputs, *(["--endomorphism","orbit"] if args.orbit else []), *(["--stride",f"{args.stride:x}"] if args.stride!=1 else []), *(["--order","reverse"] if args.order=="reverse" else []))[0]["job"]
        scope = ["--project", project, "--job", job]
        grant = local("state", "claim", *scope, "--owner", "pause-test", "--request", "claim")[0]["assignments"][0]["grant"]
        run = ["checkpoint", "run", "--state-dir", state, "--backend", args.backend, "--grant", grant, *inputs]
        slow = ["--batch-size", "128"] if mode != "bsgs" else ["--giant-batch", "1", "--target-batch", "1"]
        fast = ["--batch-size", "65536"] if mode != "bsgs" else ["--giant-batch", "16384", "--target-batch", "4", "--group-size", "8"]
        case = {"mode": mode, "layouts": [], "pause_samples": []}
        def wire(code):
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(10)
                client.connect(str(state / "control.sock"))
                client.sendall(code)
                return json.loads(client.recv(2048))
        def until(predicate, timeout=30):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                value = predicate()
                if value:
                    return value
                time.sleep(0.005)
            raise AssertionError("HIP pause condition timed out")
        def activity(expected):
            def query():
                try:
                    value = wire(b"?")
                    return value if value["state"] == expected else None
                except (FileNotFoundError, ConnectionRefusedError, ConnectionResetError):
                    return None
            return until(query)
        def rows(path):
            text = path.read_text()
            # The child can be writing its final line while this process samples.
            return [json.loads(line) for line in text[:text.rfind("\n")+1].splitlines()]
        for stage, (visibility, device, visible_count) in enumerate(layouts[:2]):
            env = os.environ.copy()
            if visibility is not None:
                env["CUDA_VISIBLE_DEVICES" if args.backend=="cuda" else "HIP_VISIBLE_DEVICES"] = visibility
            log, err = root / f"{mode}-{stage}.out", root / f"{mode}-{stage}.err"
            with log.open("w") as out, err.open("w") as error:
                process = subprocess.Popen([str(binary), *map(str, run + slow + (["--ordinal-order",args.ordinal_order if stage==0 else ("forward" if args.ordinal_order=="reverse" else "reverse")] if args.ordinal_order else []) + (["--tile-order",args.tile_order if stage==0 else ("forward" if args.tile_order=="reverse" else "reverse")] if args.tile_order else []) + ["--device", device] + (["--kernel",args.kernel if stage==0 else "direct"] if args.kernel else []))],
                                           stdout=out, stderr=error, env=env)
            try:
                live = activity("running")
                assert live["device"] == device and live["visible_devices"] == visible_count
                case["layouts"].append(live)
                # First stage waits for a real verified candidate. Later stages
                # have no early match, so the admitted batch is allowed to finish.
                if stage == 0:
                    until(lambda: any(r["type"] == "checkpoint" for r in rows(log)))
                time.sleep(0.02)
                for repeat in range(3 if stage == 0 else 1):
                    pause_begin = time.perf_counter_ns()
                    if stage == 1:
                        process.send_signal(signal.SIGUSR1)
                    else:
                        ack = wire(b"P")
                        assert ack["accepted"] and ack["state"] == "draining" and not ack["durably_paused"]
                    paused = activity("paused")
                    case["pause_samples"].append({"stage": stage, "repeat": repeat,
                        "request_to_paused_ms": (time.perf_counter_ns()-pause_begin)/1e6, "owner_observation_to_paused_ms": paused["pause_ms"]})
                    saved = local("state", "block", *scope, "--block", "0")[0]
                    assert saved["state"] == "in_progress" and saved["started"]
                    assert saved["assignment"]["grant"] == grant
                    time.sleep(0.04)
                    assert local("state", "block", *scope, "--block", "0")[0] == saved
                    if stage == 0 and repeat == 0:
                        snapshot = root / (mode + "-snapshot")
                        local("state", "backup", "--destination", snapshot)
                        local("state", "check", directory=snapshot)
                        assert local("state", "block", *scope, "--block", "0", directory=snapshot)[0]["covered"] == saved["covered"]
                        assert local("checkpoint", "results", *scope, directory=snapshot)[0]["results"]
                        restored = root / (mode + "-restored")
                        local("state", "restore", "--source", snapshot, directory=restored)
                        local("state", "check", directory=restored)
                        assert local("state", "inspect", *scope, directory=restored)[0]["quarantined"]
                        restored_run = [*run];restored_run[restored_run.index("--state-dir")+1] = restored
                        rejected = invoke(restored_run, env=env, ok=False)
                        assert "quarantined" in rejected.stderr
                    if stage == 0 and repeat < 2:
                        assert wire(b"R")["accepted"]
                        activity("running")
                        time.sleep(0.02)
                # Graceful exit while idle preserves the exact paused boundary.
                process.send_signal(signal.SIGINT if stage == 0 else signal.SIGTERM)
                assert process.wait(timeout=30) == 0, err.read_text()
                summary = rows(log)[-1]
                assert summary["type"] == "summary" and not summary["complete"]
                assert local("state", "block", *scope, "--block", "0")[0] == saved
                assert not (state / "control.sock").exists()
                local("state", "check")
                case["layouts"][-1]["summary"] = summary
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=30)
        visibility, device, visible_count = layouts[2]
        env = os.environ.copy()
        if visibility is not None:
            env["CUDA_VISIBLE_DEVICES" if args.backend=="cuda" else "HIP_VISIBLE_DEVICES"] = visibility
        completed = invoke(run + fast + (["--ordinal-order",args.ordinal_order] if args.ordinal_order else []) + (["--tile-order",args.tile_order] if args.tile_order else []) + ["--device", device] + (["--kernel","stepped"] if args.kernel else []), env=env)[-1]
        assert completed["complete"]
        if args.ordinal_order:assert completed["ordinal_order"]==args.ordinal_order
        if args.tile_order:assert completed["tile_order"]==args.tile_order
        assert int(completed["resumed_candidates" if args.orbit or args.stride!=1 or args.order=="reverse" else "resumed_ordinals" if length else "resumed_scalars"], 16) + int(completed["computed_candidates" if args.orbit or args.stride!=1 or args.order=="reverse" else "computed_ordinals" if length else "computed_scalars"], 16) == candidate_count
        matches = local("checkpoint", "results", *scope)[0]["results"]
        expected=({(n,f'{tag:02x}'+values[n,tag]) for n in seeds[:-1] for tag in (1,2)} if mode=="hash160"
                  else {(n,(bytes([tag,len(values[n,tag])])+values[n,tag].encode()+bytes(34-len(values[n,tag]))).hex()) for n in seeds[:-1] for tag in (1,2)} if mode=="vanity"
                  else set() if length else {(n,values[n]) for n in seeds[:-1]})
        if length:
            expected={(match_ordinal,f"{length:02x}{tag:02x}"+values[match_ordinal,tag]) for tag in (1,2)}
            assert all(r["minikey"]==key and int(r["scalar"],16)==minikey_scalar(key) for r in matches)
        assert {(int(r["ordinal" if length else "scalar"],16),r["target_bytes"]) for r in matches}==expected
        assert len(matches)==len(expected)
        if args.orbit or args.stride!=1 or args.order=="reverse":
            assert all(r["coordinate_space"]==("scalar-orbit-index-v1" if args.orbit else "scalar-reverse-index-v1" if args.order=="reverse" else "scalar-stride-index-v1") and int(r["scalar"],16)==scalar(int(r["candidate_index"],16)) for r in matches)
        if args.orbit:
            assert all(int(r['seed_scalar'],16)==seed(int(r['candidate_index'],16)) and
                       r['orbit_variant']==(int(r['candidate_index'],16)-1)//1048576 for r in matches)
        assert local("state", "block", *scope, "--block", "0")[0]["state"] == "finished"
        local("state", "check")
        case["completion"] = {"device": device, "visible_devices": visible_count, "summary": completed}
        report["cases"].append(case)
report["passed"] = True
args.report.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
