#!/usr/bin/env python3
"""C16: repeated, oracle-checked HIP CLI workloads with explicit durability costs."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
sys.path.insert(0, str(ROOT / "tests/oracle"))
from gpu_metrics import distribution, rates, require, validate_durable, validate_volatile
from oracle_selftest import check_source, run as oracle_run
from model import multiply, encode
from capture_environment import probe


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def save(path, value):
    # A failed or interrupted run leaves completed samples and command logs.
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


class Runner:
    def __init__(self, output, timeout):
        self.output, self.timeout, self.commands = output, timeout, []
        (output / "logs").mkdir()

    def run(self, words):
        words = list(map(str, words))
        number = len(self.commands)
        stdout, stderr = [self.output / "logs" / f"{number:04d}.{suffix}" for suffix in ("out", "err")]
        record = {"argv": words, "stdout": str(stdout.relative_to(self.output)), "stderr": str(stderr.relative_to(self.output))}
        self.commands.append(record)
        start = time.perf_counter_ns()
        # Direct file output keeps Python pipe parsing/backpressure outside the
        # timed process. Volatile CLI output volume is still part of its cost.
        try:
            with stdout.open("w") as out, stderr.open("w") as err:
                with subprocess.Popen(words, cwd=ROOT, stdout=out, stderr=err) as process:
                    # wait(timeout=...) polls in coarse intervals on POSIX. A
                    # separate watchdog allows an accurate blocking waitpid.
                    timer = threading.Timer(self.timeout, process.kill)
                    timer.start()
                    try:
                        record["exit_code"] = process.wait()
                    finally:
                        timer.cancel()
                        timer.join()
        finally:
            record["process_wall_ms"] = (time.perf_counter_ns() - start) / 1e6
            record["stdout_sha256"], record["stderr_sha256"] = sha(stdout), sha(stderr)
        require(record["exit_code"] == 0, f"command failed; see {stderr}")
        return [json.loads(line) for line in stdout.read_text().splitlines()], record


def metadata(build, binary, oracle, output, device, inventory, load_note):
    compilation = json.loads((build / "compile_commands.json").read_text())
    # Preserve exact production flags, including shared host arithmetic, without
    # exporting arbitrary environment variables or credential-bearing settings.
    production = [r for r in compilation if "/src/" in r["file"] and "/tests/" not in r["file"]]
    cache_text = (build / "CMakeCache.txt").read_text()
    home = next(line.split("=", 1)[1] for line in cache_text.splitlines() if line.startswith("CMAKE_HOME_DIRECTORY:"))
    require(Path(home).resolve() == ROOT, "build belongs to a different source checkout")
    source_files = {p for folder in ("src", "include", "kernels", "cmake") for p in (ROOT / folder).rglob("*") if p.is_file()}
    source_files.update(Path(e["file"]) for e in production)
    source_files.add(ROOT / "CMakeLists.txt")
    cache = {}
    for line in cache_text.splitlines():
        if "=" in line and ":" in line and line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:",
             "CMAKE_HIP_COMPILER:", "CMAKE_HIP_ARCHITECTURES:", "KEYHUNT_", "SQLite3_LIBRARY:", "SQLite3_INCLUDE_DIR:")):
            key, value = line.split("=", 1)
            cache[key] = value
    compiler = next((v for k, v in cache.items() if k.startswith("CMAKE_HIP_COMPILER:")), "hipcc")
    sources = ["tools/benchmark_gpu.py", "benchmarks/gpu_metrics.py", "tools/capture_environment.py"]
    return {"source_commit": probe(["git", "rev-parse", "HEAD"]),
            "working_tree": probe(["git", "status", "--porcelain"]),
            "tracked_diff_sha256": hashlib.sha256(subprocess.check_output(["git", "diff", "HEAD", "--binary"], cwd=ROOT)).hexdigest(),
            "harness_sha256": {p: sha(ROOT / p) for p in sources}, "binary_sha256": sha(binary),
            "source_files_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sorted(source_files)},
            "oracle_binary_sha256": sha(oracle), "oracle_commit": check_source(),
            "compile_commands": production, "cmake_cache": cache,
            "hip_compiler": probe([compiler, "--version"]), "cmake": probe(["cmake", "--version"]),
            "linked_libraries": probe(["ldd", str(binary)]),
            "rocm_core_version": probe(["hipconfig", "--version"]),
            "platform": platform.platform(), "python": platform.python_version(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)), "numa_policy": probe(["numactl", "--show"]),
            "cpu": probe(["lscpu", "-J"]), "visibility": {k: os.environ.get(k) for k in
                ("HIP_VISIBLE_DEVICES", "ROCR_VISIBLE_DEVICES", "CUDA_VISIBLE_DEVICES")},
            "inventory": inventory, "selected_device": device,
            "storage": probe(["findmnt", "-T", str(output), "-J", "-o", "TARGET,SOURCE,FSTYPE,OPTIONS"]),
            "durability": {"journal_mode": "WAL", "synchronous": "FULL", "enforced_by": "production journal constructor",
                           "timed_seconds": 10, "every_batch_seconds": 0, "remote_sync": "not used"},
            "other_load": load_note, "smi_before": smi(device)}


def smi(device):
    # SMI ordinals are not HIP ordinals under visibility remapping. Select by BDF.
    return probe(["amd-smi", "metric", "--gpu", device["pci_bus_id"], "--clock", "--power", "--usage", "--json"])


def make_case(args, mode, workload, output, oracle, table_info):
    begin = (1 << 128) + 17
    width = args.batches * (args.batch_size if mode == "xpoint" else args.m * args.giant_batch) - 1
    require(width >= 3, "workload must contain at least three scalars")
    end = begin + width
    require(workload != "dense-prefix" or width >= 4, "dense prefix needs four scalars")
    # A short saturated prefix followed by a sparse tail exposes retry policies
    # that permanently shrink all later work after one overflowing launch.
    seeds = (list(range(begin, begin+4)) if workload == "dense-prefix" else
             [begin, begin + width // 2, end - 1] if workload == "boundary-3" else
             list(range(1, 33 if workload == "no-match-32" else 2)))
    public = oracle_run(oracle, [f"pub {k:064x}" for k in seeds])
    # Validate the pinned executable against a separate Python affine model for
    # every benchmark target. X-only no-match partners n-k are outside this range.
    require(public == [encode(multiply(k)) for k in seeds], "independent oracle disagreement")
    values = [p[2:66] if mode == "xpoint" else p for p in public]
    targets = output / f"{mode}-{workload}.txt"
    targets.write_text("\n".join(values) + "\n")
    target_batch = min(len(values), args.target_batch)
    require(mode != "bsgs" or args.giant_batch * target_batch <= 1048576, "giant/target batch exceeds executor bound")
    tag = b"xpoint-v1\0" if mode == "xpoint" else b"bsgs-targets-v1\0"
    digest = hashlib.sha256(tag + b"".join(bytes.fromhex(v) for v in sorted(values))).hexdigest()
    return {"id": f"{mode}-{workload}", "mode": mode, "workload": workload,
            "begin": f"{begin:x}", "end_exclusive": f"{end:x}", "target_count": len(values),
            "targets": str(targets), "target_file_sha256": sha(targets), "target_digest": digest,
            "target_values": sorted(values), "expected_matches": [[f"{k:x}", v] for k, v in zip(seeds, values) if begin <= k < end],
            "m": args.m if mode == "bsgs" else None,
            "table_checksum": table_info["checksum"] if mode == "bsgs" else None,
            "geometry": (["--batch-size", str(args.batch_size), "--kernel", args.kernel] if mode == "xpoint" else
                         ["--giant-batch", str(args.giant_batch), "--target-batch", str(target_batch), "--group-size", args.group_size]),
            "candidate_capacity": args.candidate_capacity}


def execute(args, runner, case, variant, round_number, binary, table, device):
    inputs = ["--targets", case["targets"]] + (["--table", table] if case["mode"] == "bsgs" else [])
    tuning = ["--backend", "hip", "--device", args.device, "--candidate-capacity", args.candidate_capacity, *case["geometry"]]
    state = None
    if variant == "volatile":
        command = [binary, case["mode"], "--range", f'{case["begin"]}:{case["end_exclusive"]}', *inputs, *tuning]
    else:
        state = runner.output / "state" / f'{case["id"]}-{variant}-{round_number}'
        def local(family, action, *words):
            return runner.run([binary, family, action, "--state-dir", state, *words])[0]
        project = local("state", "project-create", "--name", "C16 synthetic benchmark")[0]["project"]
        width = int(case["end_exclusive"], 16) - int(case["begin"], 16)
        created = local("checkpoint", "create", "--project", project, "--mode", case["mode"],
                        "--range", f'{case["begin"]}:{case["end_exclusive"]}', "--block-width", f"{width:x}", *inputs)[0]
        require(created["target_digest"] == case["target_digest"], "checkpoint target digest mismatch")
        scope = ["--project", project, "--job", created["job"]]
        grant = local("state", "claim", *scope, "--owner", "benchmark", "--request", "claim")[0]["assignments"][0]["grant"]
        command = [binary, "checkpoint", "run", "--state-dir", state, "--grant", grant, *inputs, *tuning,
                   "--checkpoint-seconds", 10 if variant == "timed" else 0]
    rows, invocation = runner.run(command)
    if state is None:
        metrics = validate_volatile(rows, case, device["uuid"])
    else:
        local("state", "check")
        block = local("state", "block", *scope, "--block", "0")[0]
        matches = local("checkpoint", "results", *scope)[0]["results"]
        metrics = validate_durable(rows, case, device["uuid"], block, matches)
    return {"round": round_number, "warmup": round_number < 0, "variant": variant,
            "command": invocation, "summary": rows[-1],
            "metrics": rates(metrics, case, invocation["process_wall_ms"], rows[-1])}


def summarize(samples):
    measured = [s for s in samples if not s["warmup"]]
    keys = [k for k, v in measured[0]["metrics"].items() if isinstance(v, (int, float))]
    return {key: distribution([s["metrics"][key] for s in measured]) for key in keys}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory outside the checkout; retains journals/logs")
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--batches", type=int, default=128)
    parser.add_argument("--batch-size", type=int, default=1048576)
    parser.add_argument("--giant-batch", type=int, default=32768)
    parser.add_argument("--target-batch", type=int, default=32)
    parser.add_argument("--m", type=int, default=65537)
    parser.add_argument("--candidate-capacity", type=int, default=1024)
    parser.add_argument("--kernel", choices=("stepped", "direct"), default="stepped")
    parser.add_argument("--group-size", choices=("auto", "1", "8"), default="auto")
    parser.add_argument("--modes", choices=("xpoint", "bsgs"), nargs="+", default=["xpoint", "bsgs"])
    parser.add_argument("--workloads", choices=("no-match-1", "boundary-3", "no-match-32"), nargs="+", default=["no-match-1", "boundary-3", "no-match-32"])
    parser.add_argument("--variants", choices=("volatile", "timed", "every-batch"), nargs="+", default=["volatile", "timed", "every-batch"])
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--other-load", default="Unreserved host; concurrent sibling/device load is unknown.")
    args = parser.parse_args()
    bounds = {"repeats": (5, 100), "batches": (1, 1000000), "batch_size": (4, 1048576),
              "giant_batch": (1, 1048576), "target_batch": (1, 64), "m": (2, 1048576),
              "candidate_capacity": (1, 65536), "timeout": (1, 86400)}
    for name, (low, high) in bounds.items():
        if not low <= getattr(args, name) <= high:
            parser.error(f"{name} must be in [{low},{high}]")
    for name in ("modes", "workloads", "variants"):
        if len(set(getattr(args, name))) != len(getattr(args, name)):
            parser.error(f"duplicate {name}")
    output, build = args.output_dir.resolve(), args.build_dir.resolve()
    if output == ROOT or ROOT in output.parents or output.exists():
        parser.error("output-dir must be new and outside this checkout")
    if args.device < 0:
        parser.error("device must be nonnegative")
    output.mkdir(parents=True, mode=0o700)
    runner = Runner(output, args.timeout)
    binary, oracle = build / "keyhunt", build / "secp256k1_oracle"
    report = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": False,
              "scope": "single logical HIP device; fresh processes, one excluded warm-up round; rotating durability order",
              "timing": "process includes input/preparation, execution, output and cleanup; job creation, table build and post-run audits are separate",
              "unmeasured": ["remote sync", "queue-empty time", "continuous clocks", "multi-device scaling", "pause latency"],
              "options": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "cases": [], "commands": runner.commands, "failures": []}
    try:
        inventory = runner.run([binary, "devices", "--backend", "hip"])[0][0]
        device = next(d for d in inventory["devices"] if d["ordinal"] == args.device)
        report["metadata"] = metadata(build, binary, oracle, output, device, inventory, args.other_load)
        table, table_info = output / "babies.khb", {}
        if "bsgs" in args.modes:
            rows, command = runner.run([binary, "bsgs-table", "build", "--m", args.m, "--output", table])
            table_info = rows[0]
            report["table_build"] = {"record": table_info, "command": command, "file_sha256": sha(table)}
        for mode in args.modes:
            for workload in args.workloads:
                case = make_case(args, mode, workload, output, oracle, table_info)
                case["samples"] = []
                report["cases"].append(case)
                for repeat in range(-1, args.repeats):
                    # Each variant sees identical work. Rotating order avoids
                    # consistently assigning cold or hotter device state to one.
                    offset = (repeat + 1) % len(args.variants)
                    order = args.variants[offset:] + args.variants[:offset]
                    for variant in order:
                        sample = execute(args, runner, case, variant, repeat, binary, table, device)
                        case["samples"].append(sample)
                        save(output / "report.json", report)
                    print(f'{case["id"]}: round {repeat} checked', flush=True)
                case["statistics"] = {v: summarize([s for s in case["samples"] if s["variant"] == v]) for v in args.variants}
        report["metadata"]["smi_after"] = smi(device)
        report["passed"] = True
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration, subprocess.SubprocessError) as error:
        report["failures"].append(f"{type(error).__name__}: {error}")
    finally:
        save(output / "report.json", report)
    print(f'Benchmark {"passed" if report["passed"] else "FAILED"}: {output / "report.json"}')
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
