# C16 GPU profiling and reproducible benchmarks

C16 records the pre-tuning portable HIP baseline. Current kernel and batching
changes are measured separately in [C17 tuning](HIP_TUNING.md). The [earlier audit](audits/C13_AUDIT.md) found that startup and different
CLI output volumes can dominate small durability comparisons; this harness records
those costs explicitly. See [checkpoint metrics](CHECKPOINTS.md#c16-execution-metrics)
for the durable accounting contract. The same harness now accepts
`--backend cuda --build-dir build/cuda-h200`; use a fresh `/var/tmp` output
directory on the H200 host. ROCm profiler sections below apply only to HIP.
C18/C19 tuning and C20 fleet results are linked in the
[current performance guidance](OPERATIONS.md#interpreting-performance); the C16
tables remain historical portable-HIP measurements.

## Reproduce an end-to-end run

Build first, then choose a **new directory outside the checkout** on the filesystem
whose durability cost you intend to measure. The directory retains synthetic
inputs, the table, separate journals, command logs, and an atomic `report.json`.
Never use a live job's journal. No downloads, credentials, network service, clock,
power, or partition changes are involved.

```sh
cmake --build --preset hip-release -j 8
python3 tools/benchmark_gpu.py --build-dir build/hip-release \
  --output-dir /tmp/keyhunt-c16-run --device 0 --repeats 5 --batches 128
```

The default matrix covers xpoint and BSGS, one and 32 no-match targets, three
matches at the first/middle/final scalar, and volatile, ten-second checkpoint,
and every-batch checkpoint variants. Each case has one complete excluded warm-up
round and at least five measured repetitions; variant order rotates every round.
Each execution is a **fresh process**, not a persistent warm executor. The
executor totals inside each process exclude startup and retain all attempts.
The final interval has a scalar tail. All starts exceed 64 bits. BSGS defaults to
`m=65537`, 32768 giants, up to 32 targets per launch; xpoint uses 1048576 scalars
per batch. `--help` exposes geometry, grouping, candidate capacity, and subsets.

Every target comes from pinned libsecp256k1 and is checked against the independent
Python affine model. No-match targets have known scalars outside the interval;
their X-only partners are also outside it. Volatile receipts must form an exact,
ordered interval with every BSGS target processed. Durable runs reopen and audit
the journal, require the exact stored coverage union, and compare stored matches
with the oracle. Overflow costs remain visible and cannot inflate useful work.
A failed command, missing summary, wrong device/input, incomplete coverage, or
unexpected match fails the report and excludes that sample from statistics.

For a small real-device overflow acceptance run:

```sh
python3 tools/benchmark_gpu.py --build-dir build/hip-release \
  --output-dir /tmp/keyhunt-c16-overflow --batches 1 --batch-size 1024 \
  --giant-batch 16 --m 17 --workloads boundary-3 --candidate-capacity 1
```

## Metrics and comparison rules

| Field | Meaning |
| --- | --- |
| `process_wall_ms` | Parent monotonic timer around execution, including preparation, output and cleanup; excludes job/claim setup, table build and post-run audits |
| `wall_ms` | Child command timer; see the per-command preparation/cleanup boundary |
| `executor_wall_ms` | Sum of submit-to-take times, including failed overflow attempts; excludes preparation and journal work |
| `kernel_ms`, `download_ms` | Sums of HIP event durations, including overflow attempts |
| `seed_ms`, `verification_ms` | Host executor point-seed and candidate-verification work |
| `checkpoint_ms`, `revalidation_ms` | Commit API cost and separate owner-side match checks |
| `preparation_ms` | Startup through first submission; contains table loading and executor creation |
| `table_upload_ms` | BSGS upload event timing, nested within preparation |
| `replay_kernel_ms`, `overflow_device_steps` | Discarded overflow attempt cost/work; process-crash replay is not simulated |
| `scalar_coverage` | Exact scalar interval width, counted once across all BSGS targets |
| `useful_device_steps` | Xpoint scalars or BSGS **target giant steps**, excluding overflow |
| `process_scalars_per_s` | Scalar coverage divided by execution-process wall time |
| `process_useful_steps_per_s` | Useful device work divided by the same wall time |
| `kernel_attempted_steps_per_s` | All attempted device work divided by kernel event time; not useful job throughput |

Counts in normalized metrics are decimal strings to preserve wide integers.
Original CLI counts retain their documented hexadecimal formats. Timings are
milliseconds; rates are per second. Statistics retain every sample and report
median, min/max, spread, and median absolute deviation. Never multiply scalar
coverage by the target count or describe BSGS coverage as public-key evaluations.
Nested timers cannot be summed as disjoint phases.

The report captures commit, working-tree status/diff hash, harness and executable
hashes, production compile commands, compiler/runtime versions, visible device
inventory, UUID/BDF and physical mapping, partition modes, free/total HBM,
CPU affinity/NUMA policy, before/after clock/power/utilization snapshots, input
hashes and geometry. SMI selection uses PCI BDF because HIP visibility can renumber
ordinals. Optional probe failures remain explicit. `--other-load` records operator
knowledge of sibling load; snapshots do not prove an exclusive or idle machine.

WAL/FULL are enforced by the production journal constructor. Filesystem and mount
options are recorded. Timed runs shorter than ten seconds usually measure a final
commit, not periodic ten-second commits. For cadence experiments increase
`--batches` until the report contains several no-match checkpoints. Match-bearing
runs may commit early to persist results. CLI output differs by variant, so the
process-time difference is **not an isolated SQLite cost**. Table build and job
setup have their own raw commands and timing records.

This harness measures one logical device at a time. Remote sync, queue exhaustion,
continuous utilization, pause latency, package scaling and crash replay are
explicitly unmeasured, not silently reported as zero. The existing
`tools/measure_c14_pause.py` measures pause separately. Compare identical inputs,
durability, geometry, UUID/partition, flags, and CPU placement; record intentional
variant differences. Independent runs are not paired speedup evidence. A C17
optimization needs alternating baseline/candidate trials and unchanged correctness
and pause/replay gates; C16 itself makes no optimization claim.

## Validation logic

`tests/unit/gpu_metrics.py` is registered as `gpu_benchmark_metrics` in CPU and HIP
CTest. It checks wide intervals, partial BSGS tails and target multiplicity,
duplicate/missing target groups, false completion, device/table mismatch,
missing/duplicate matches, overflow costs, durable unions, invalid timings and
summary statistics. Performance measurements remain opt-in and have no timing
threshold in correctness CI.

## Separate profiler and compiler capture

Use a passed benchmark report whose inputs are still present. Profiling repeats
one volatile case, validates its coverage again, and never adds its duration to
benchmark statistics. Availability/version/help output, raw trace CSVs, and a
hashed artifact manifest are retained. Optional counter failures fail explicitly.
The profiler supports the installed ROCprofiler-SDK command set; its help and
available counters are queried before collecting data. See AMD's
[rocprofv3 reference](https://rocm.docs.amd.com/projects/rocprofiler-sdk/en/latest/how-to/using-rocprofv3.html).

```sh
python3 tools/profile_gpu.py --benchmark /tmp/keyhunt-c16-run/report.json \
  --case xpoint-no-match-1 --output-dir /tmp/keyhunt-c16-xpoint-profile --resources
python3 tools/profile_gpu.py --benchmark /tmp/keyhunt-c16-run/report.json \
  --case bsgs-no-match-1 --output-dir /tmp/keyhunt-c16-bsgs-profile
```

`--counter NAME` may be repeated for a compatible single-pass counter set; that
collection is a second instrumented run, separate from timeline tracing. Choose
names from the captured `available-counters.out`, not a different SDK's list.
Trace kernel counts must agree with application search receipts. On this SDK,
small copies and buffer clears can appear as internal `__amd_rocclr_*` kernels;
these remain in the artifact but are excluded from the search-launch comparison.
An absent SDMA copy CSV does not imply zero transfers: inspect HIP API calls and
internal copy kernels too.

`--resources` reuses recorded production compiler flags to write standalone
AMDGPU assembly and `-Rpass-analysis=kernel-resource-usage` remarks for xpoint and
BSGS. It verifies the current source hashes match the benchmark and leaves build
objects and the executable intact. Assembly includes code-object metadata;
remarks report registers, scratch/spills, LDS and estimated occupancy. These are
compiler estimates, not measured occupancy or proof of a performance bottleneck.
Benchmark metadata rejects a build belonging to another source checkout. Rebuild
before measuring: source and binary hashes identify both artifacts, but do not
prove that an arbitrary pre-existing executable was freshly built from that source.

## Accepted C16 evidence and findings

Measured source: `7ae5cbf` (implementation commits `f48f16d`, `e54705b`,
`126ae8d`), 2026-09-30. The measurement checkout was clean. Device: one MI300X,
SPX/NPS1, 304 CUs, wave64, PCI `0000:3d:00.0`, HIP UUID
`30393337633864376335303031303365`, visible as ordinal 0 under
`HIP_VISIBLE_DEVICES=1`. HIP runtime/driver: 71526333; ROCprofiler-SDK: 1.3.5;
ROCm core: 10.0.0; AMD clang: 23.0.0git. The runtime journal filesystem was local
ext4 under `/tmp`, with WAL/FULL enforced. No device settings were changed.
This host was unreserved. C16 regression jobs completed before measurement;
an independent C15 audit had been observed on another physical device.

[Full validation](baselines/C16_VALIDATION.json) records 32/32 CPU release,
58/58 HIP/coordinator release, 5/5 focused debug and 3/3 focused ASan/UBSan tests.
The [matrix](baselines/C16_MEASUREMENTS.json) retains **90 measured samples and
18 warm-ups**: both modes, three target workloads and three durability policies.
The [forced-overflow run](baselines/C16_OVERFLOW.json) retains 30 measured samples
and six warm-ups, with exact matches/coverage in both modes and all policies.
That smoke run preceded the final harness commits; its report retains the actual
working-tree provenance. The final matrix and cadence reports identify the clean
measured revision and full source hashes.

The matrix uses 512 batches: 536,870,911 xpoint scalars or
1,099,528,404,991 BSGS scalar positions per run. Representative **ten-second
checkpoint policy** medians are below. BSGS uses `m=65537`; its coverage rates
are not public-key evaluation rates. All raw samples and spread remain in the
linked JSON, including volatile and every-batch variants.

| Mode / targets | Process scalar coverage/s | Executor scalar coverage/s |
| --- | ---: | ---: |
| xpoint / no-match-1 | 541.345 million | 1.422 billion |
| xpoint / boundary-3 | 521.639 million | 1.352 billion |
| xpoint / no-match-32 | 326.587 million | 523.192 million |
| bsgs / no-match-1 | 792.546 billion | 2.975 trillion |
| bsgs / boundary-3 | 749.309 billion | 2.426 trillion |
| bsgs / no-match-32 | 650.125 billion | 1.621 trillion |

Startup remains material in these short cases. The separate
[long cadence trial](baselines/C16_CADENCE.json) uses 32,768 xpoint batches,
34,359,738,367 scalars, and one known no-match target. Five measured samples after
warm-up took 24.267–24.443 s (median **24.393 s**) and each produced **three
checkpoints**, exercising two periodic boundaries plus the final commit.
Median process throughput was **1.409 billion scalars/s**, versus 1.500 billion/s
within the executor. Median preparation was 545.3 ms; all three commit API calls
together took a median 1.116 ms. This is a cadence observation, not a claim that
commit cost alone explains the difference between durability variants.

The [profiling manifest and compiler remarks](baselines/C16_PROFILES.json)
record separate validated xpoint and BSGS traces, each with **512 search
launches**, plus **514 internal copy kernels and 1,024 clear kernels**. The
optional `SQ_WAVES` counter collection also passed. Internal dispatches explain
why total trace kernel count must not be equated to useful search launches.
Current compiler estimates retain substantial private-memory pressure:

| Compiled kernel | VGPRs | Scratch bytes/lane | Reported VGPR spills | Reported SGPR spills | Estimated waves/SIMD |
| --- | ---: | ---: | ---: | ---: | ---: |
| xpoint stepped `<true>` (observed one-target dispatch) | 128 | 260 | 220 | 0 | 4 |
| BSGS group 1 | 248 | 208 | 0 | 188 | 2 |
| BSGS group 8 | 248 | 1232 | 0 | 248 | 2 |

These remarks and the retained assembly support investigating the existing
[A13 launch-bound finding](audits/C11_AUDIT.md#a13--declare-the-actual-128-thread-xpoint-launch-bound)
in C17; C16 changes no kernels and claims no optimization speedup. Other open
findings include permanent overflow batch reduction and capacity-sized candidate
transfers. Maximum allocation fields are the largest **executor receipt** sizes,
not a measured peak across table construction and every transient allocation;
the table-build record separately reports its host peak estimate.

[Pause samples](baselines/C16_PAUSE.json), collected separately with the C14
harness on the same selected device, contain five warm samples for each mode and
small/default geometry. Median request-to-durably-paused latency spans
0.412–1.040 ms; the largest observed sample is 1.123 ms. These short synthetic
runs establish observed latency only, not a universal bound. The standalone
pause path is distinct from the still-open
[C15 supervisor control findings](audits/C15_AUDIT.md).

[Raw logs, trace CSVs, assembly and resource remarks](baselines/C16_RAW_LOGS.tar.gz)
are archived with relative `matrix/`, `cadence/`, `overflow/`, and `profile-*/`
prefixes. Per-command paths and hashes in the JSON resolve within those prefixes.
The archive includes no runtime databases, WAL files, credentials or BSGS table
cache. Synthetic inputs and the table are reproducible from the recorded commands,
scalars, hashes and geometry. Original run directories remain under `/tmp`.

The trace manifests include SHA-256 hashes for each artifact. The validation
manifest hashes every evidence file and the archive. Performance is scoped to
this device, toolchain, small table, target sets and durability settings; package
scaling, NVIDIA, supervisor throughput, and remote sync are outside C16 evidence.
