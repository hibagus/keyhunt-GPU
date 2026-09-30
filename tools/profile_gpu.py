#!/usr/bin/env python3
"""Capture ROCm traces and compiler resources separately from C16 timing trials."""
import argparse
import csv
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

from benchmark_gpu import ROOT, save, sha
sys.path.insert(0, str(ROOT / "benchmarks"))
from gpu_metrics import require, validate_volatile


def resource_command(entry, output):
    """Reuse production flags, replacing all object/dependency output paths."""
    tokens = entry.get("arguments") or shlex.split(entry["command"])
    command, skip = [], False
    for token in tokens:
        if skip:
            skip = False
        elif token in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif token not in ("-c", "-MD", "-MMD", "-MP"):
            command.append(token)
    return command + ["--offload-device-only", "-S", "-Rpass-analysis=kernel-resource-usage", "-o", str(output)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True, help="passed report.json with retained synthetic inputs")
    parser.add_argument("--case", required=True, help="e.g. xpoint-no-match-1")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--rocprof", default="rocprofv3")
    parser.add_argument("--counter", action="append", default=[], help="optional single-pass counter; repeat for multiple counters")
    parser.add_argument("--resources", action="store_true", help="also compile device assembly/resource remarks using recorded flags")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists() or output == ROOT or ROOT in output.parents:
        parser.error("output-dir must be new and outside the checkout")
    output.mkdir(parents=True, mode=0o700)
    report = {"schema_version": 1, "recorded_utc": datetime.now(timezone.utc).isoformat(), "passed": False,
              "timing_sample": False, "commands": [], "failures": [], "artifacts": []}

    def run(words, name, cwd=ROOT, timeout=120):
        command = list(map(str, words))
        record = {"argv": command, "cwd": str(cwd)}
        report["commands"].append(record)
        out, err = output / (name + ".out"), output / (name + ".err")
        # Profile durations are deliberately excluded from timing statistics.
        with out.open("w") as stdout, err.open("w") as stderr:
            result = subprocess.run(command, cwd=cwd, stdout=stdout, stderr=stderr, timeout=timeout)
        record.update(exit_code=result.returncode, stdout=out.name, stderr=err.name)
        require(result.returncode == 0, f"profile command failed; inspect {err}")
        return out.read_text()

    try:
        benchmark = json.loads(args.benchmark.read_text())
        require(benchmark["passed"] and benchmark["schema_version"] == 1, "benchmark did not pass")
        case = next(c for c in benchmark["cases"] if c["id"] == args.case)
        sample = next(s for s in case["samples"] if s["variant"] == "volatile" and not s["warmup"])
        command = sample["command"]["argv"]
        meta = benchmark["metadata"]
        require(sha(command[0]) == meta["binary_sha256"] and sha(case["targets"]) == case["target_file_sha256"],
                "binary/targets differ from benchmark")
        if case["mode"] == "bsgs":
            table = command[command.index("--table") + 1]
            require(sha(table) == benchmark["table_build"]["file_sha256"], "table differs from benchmark")
        require(all(os.environ.get(k) == v for k, v in meta["visibility"].items()), "device visibility changed")
        inventory = json.loads(run([command[0], "devices", "--backend", "hip"], "inventory"))
        device = next(d for d in inventory["devices"] if d["ordinal"] == meta["selected_device"]["ordinal"])
        require(all(device[k] == meta["selected_device"][k] for k in ("uuid", "architecture", "compute_partition", "memory_partition")),
                "profile device differs from benchmark")
        report.update(benchmark_sha256=sha(args.benchmark), binary_sha256=meta["binary_sha256"],
                      case=case["id"], selected_device=device, cpu_affinity=sorted(os.sched_getaffinity(0)))
        run([args.rocprof, "--version"], "profiler-version")
        help_text = run([args.rocprof, "--help"], "profiler-help")
        for flag in ("--kernel-trace", "--memory-copy-trace", "--output-directory", "--list-avail"):
            require(flag in help_text, f"installed profiler lacks {flag}")
        # Availability is captured for this installed SDK/device; no hardcoded
        # counter names or assumed physical-card resource counts.
        run([args.rocprof, "--list-avail"], "available-counters")
        trace_dir = output / "trace"
        prefix = [args.rocprof, "--kernel-trace", "--memory-copy-trace", "--hip-runtime-trace",
                  "--scratch-memory-trace", "--output-format", "csv", "--output-directory", trace_dir,
                  "--output-file", "capture"]
        stdout = run([*prefix, "--", *command], "trace-command", timeout=300)
        rows = [json.loads(line) for line in stdout.splitlines() if line.startswith('{"type":')]
        validate_volatile(rows, case, device["uuid"])
        traces = sorted(trace_dir.rglob("*kernel_trace.csv"))
        require(bool(traces), "profiler produced no kernel trace")
        records = []
        for trace in traces:
            with trace.open() as stream:
                records.extend(csv.DictReader(stream))
        # HIP may implement small copies and memset with internal kernels.
        # Preserve them in the trace, but compare only actual search launches.
        needle = "keyhunt::gpu::xpoint_" if case["mode"] == "xpoint" else "keyhunt::gpu::bsgs_search"
        search_records = [r for r in records if needle in r["Kernel_Name"]]
        require(len(search_records) == rows[-1]["launch_count"], "trace search dispatch count differs from validated search")
        report["trace_search_launches"] = len(search_records)
        report["trace_kernel_counts"] = dict(Counter(r["Kernel_Name"] for r in records))
        report["search_summary"] = rows[-1]
        if args.counter:
            # Keep expensive counter collection in its own invocation. SDK
            # rejection of unsupported/incompatible counters remains a failure.
            counter_stdout = run([args.rocprof, "--pmc", *args.counter, "--output-format", "csv",
                                  "--output-directory", output / "counters", "--output-file", "capture", "--", *command],
                                 "counter-command", timeout=300)
            validate_volatile([json.loads(line) for line in counter_stdout.splitlines() if line.startswith('{"type":')], case, device["uuid"])
            require(bool(list((output / "counters").rglob("*counter_collection.csv"))), "no counter collection output")
        if args.resources:
            # Recompile to a separate directory, preserving the tested executable
            # and object files. The resulting assembly embeds AMD code metadata.
            require(bool(meta.get("source_files_sha256")), "benchmark lacks source hashes; rerun with current harness")
            require(all(sha(ROOT / p) == digest for p, digest in meta["source_files_sha256"].items()),
                    "current source differs from benchmark; cannot attribute compiler resources")
            entries = [e for e in meta["compile_commands"] if e["file"].endswith(("/xpoint.hip", "/bsgs_search.hip"))
                       and "keyhunt_backend.dir" in (e.get("command") or " ".join(e["arguments"]))]
            require(len(entries) == 2, "expected two production search translation units")
            report["resource_source_commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
            report["resource_working_tree"] = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
            for entry in entries:
                name = Path(entry["file"]).stem
                assembly = output / (name + ".s")
                run(resource_command(entry, assembly), name + "-resources", cwd=entry["directory"], timeout=300)
                require(assembly.is_file() and "VGPR" in (output / (name + "-resources.err")).read_text(), "missing compiler resource evidence")
        report["passed"] = True
    except (OSError, ValueError, RuntimeError, KeyError, StopIteration, subprocess.SubprocessError) as error:
        report["failures"].append(f"{type(error).__name__}: {error}")
    finally:
        for path in sorted(output.rglob("*")):
            if path.is_file():
                report["artifacts"].append({"path": str(path.relative_to(output)), "bytes": path.stat().st_size, "sha256": sha(path)})
        save(output / "report.json", report)
    print(f'Profile {"passed" if report["passed"] else "FAILED"}: {output / "report.json"}')
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
