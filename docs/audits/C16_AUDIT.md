# C16 audit: profiling evidence and mixed BSGS dispatch accounting

Audited revision: `92486d8`, **2026-09-30**, using an isolated checkout and fresh
builds. Concurrent C17 implementation is outside this review. Production source
and implementation plans were not changed by the audit.

**All 58 HIP/coordinator tests and 42 CPU/coordinator tests passed.** Independent
checks of the published C16 benchmark evidence also passed. One new measurement
defect was reproduced on MI300X: a durable BSGS run can execute both grouping
kernels while its reported `actual_groups` contains only the last group.
Its coverage and results remain correct.

## Evidence checked

- [Independent validation and build fingerprints](C16_AUDIT_VALIDATION.json).
  Complete suite times were 326.92 s for HIP and 66.59 s for CPU. Both builds
  enabled the coordinator and its private localhost Apache/mTLS integration tests.
- [Audit probes](C16_AUDIT_PROBES.json) and
  [reproduction tool](../../tools/audit_c16.py).
- All six published C16 evidence-file hashes matched the validation manifest.
  The audit checked **767 command stdout/stderr hash pairs**, reconstructed the
  known targets with the independent Python affine model, checked raw coverage
  and match receipts, and recalculated rates and **492 statistic distributions**.
- Those checks cover **125 measured samples plus 25 warm-ups**: the original
  matrix, forced-overflow cases and long checkpoint-cadence run. These are checks
  of retained evidence, not a claim to have rerun that entire measurement matrix.
- Every archived profiler artifact hash and all three recorded kernel-count
  maps matched. Each trace had 512 search launches. Compiler remarks remain
  compiler estimates; this audit does not relabel them as measured occupancy.
- Live audit work used one MI300X, SPX/NPS1, 304 CUs, wave64, PCI `0000:1b:00.0`,
  runtime ordinal 0 under `HIP_VISIBLE_DEVICES=0`. HIP runtime/driver `71526333`,
  Release `gfx942` compilation. The original C16 measurements used a different
  physical MI300X, PCI `0000:3d:00.0`; the two sets are not paired speedup trials.
  No power, clock or partition changes were made. The host was unreserved.

## A19 — mixed durable BSGS kernels are reported as only the final group

Priority: **P2, reproduced profiling/accounting defect**. Frozen source:
[checkpoint.cpp](../../src/storage/checkpoint.cpp):240 assigns
`summary.bsgs_group_size=result.group_size` after every completed attempt.
[gpu_metrics.py](../../benchmarks/gpu_metrics.py):140 then converts this final
value into the run-level list `actual_groups`.

Automatic grouping depends on both the giant count and the current target subset.
Different target subsets in one scalar tile can therefore select different
kernels. Last-dispatch information cannot describe the entire run.

The live reproduction used `m=17`, 8,192 giants per tile, 32 known no-match
targets, `--target-batch 31`, automatic grouping and 278,527 scalar positions.
The unmodified C16 harness passed five repetitions plus a warm-up for each of
volatile and timed durability. Both variants completed exact coverage and
524,288 useful target giant steps.

| Dispatch in each of two tiles | Targets | Actual group | Giant steps per dispatch |
| --- | ---: | ---: | ---: |
| First subset | 31 | 8 | 253,952 |
| Final subset | 1 | 1 | 8,192 |

The volatile report correctly contains `actual_groups:[1,8]`. Every durable
sample contains `actual_groups:[1]`. A **separate fresh durable grant**, captured
with ROCprofiler, confirmed two `bsgs_search<8u>` and two `bsgs_search<1u>` launches.
Its summary still reported `bsgs_group_size:1`. The profiler run is excluded from
performance statistics, and its journal integrity check passed.

Group 8 performs **96.875% of the useful giant-step work** in this case. Reporting
only group 1 can associate the aggregate timing with the wrong register/scratch
profile and obscure the effect of a grouping-policy change. This does not
invalidate the original C16 matrix: its recorded geometry uses one grouping
choice per case. Arbitrary final tiles and overflow subgroup splitting can also
produce mixed dispatches, so the issue matters beyond this particular fixture.

Record the set of observed groups and preferably launch counts, attempted/useful
steps and kernel time per group. If the existing scalar field is retained, label
it explicitly as the last group and stop converting it into a complete group
list. Add mixed-subset and small-final-tile cases to the metric tests. Preserve
the existing overflow accounting and count scalar coverage once across targets.

## Fresh periodic-checkpoint measurement

A separate run of the unmodified C16 harness on the audit GPU completed one
excluded warm-up and five measured xpoint runs. Each searched **34,359,738,367
scalars** against one known no-match target, using 32,768 batches, the stepped
kernel and the ten-second checkpoint policy. Every run produced **three commits**:
two periodic checkpoints and the final commit. An additional audit of the raw
checkpoint receipts confirmed their exact contiguous interval union and summed
transaction time. Stored coverage and results passed the harness's journal audit.

| Metric | Median across five measured runs |
| --- | ---: |
| Whole execution-process time | **24.037 s** |
| Whole-process scalar throughput | **1.429 billion/s** |
| Accumulated executor scalar throughput | **1.520 billion/s** |
| Preparation time | 462.508 ms |
| Total time in three checkpoint API calls | 1.676 ms |

Process times ranged from 24.024 to 24.121 s. Process timing includes startup,
execution, output and cleanup; initial job creation and subsequent integrity
checks are separate. Executor timing excludes preparation and journal work.
These are finite-run measurements on one unreserved GPU, not multi-hour or
multi-device utilization figures. The checkpoint API cost alone does not explain
the gap between process and executor time, and these results do not establish a
speedup against C15 or the original C16 measurements on another physical GPU.

The [fresh cadence report](C16_AUDIT_CADENCE.json) retains all samples and metadata.
[Raw audit logs and the durable dispatch trace](C16_AUDIT_RAW_LOGS.tar.gz) retain
relative `cadence/` and `mixed/` paths; no runtime databases or table cache are
included. Reproduce the cadence run with the frozen checkout's
`tools/benchmark_gpu.py --build-dir BUILD --output-dir NEW_DIR --modes xpoint
--workloads no-match-1 --variants timed --repeats 5 --batches 32768`, under the same
GPU visibility. Build before measuring and keep the output outside the checkout.

## What the traces say about optimization priority

The original one-target traces contain 1,538 internal copy/clear dispatches for
512 search launches. Counts alone exaggerate their contribution. Summing each
dispatch's end minus start gives the following instrumented durations. ROCprofiler
defines these timestamps in nanoseconds; the table converts them to milliseconds.
[AMD ROCprofiler-SDK field definitions](https://rocmdocs.amd.com/projects/rocprofiler-sdk/en/latest/how-to/using-rocprofv3.html)

| Trace | Search dispatch time | Copy + clear dispatch time | Internal share of summed dispatch time |
| --- | ---: | ---: | ---: |
| Xpoint | 337.674 ms | 6.334 ms | **1.84%** |
| BSGS | 320.514 ms | 3.989 ms | **1.23%** |

These fractions exclude CPU scheduling gaps and other host costs. They describe
the recorded traces and are not a bound or prediction for unprofiled end-to-end
speedup. Large candidate capacities and tiny batches can behave differently.
The existing A14 transfer finding remains valid, but dispatch count alone is
insufficient evidence to prioritize it ahead of register spilling in these cases.

For C17, the strongest existing measured experiment is still **A13: declare the
actual 128-thread xpoint launch bound**. The baseline compiler reports 220 VGPR
spills and 260 scratch bytes per lane for the small-target stepped kernel.
The earlier isolated experiment reached about 2.13 billion one-target scalars/s
and 1.48–1.51× kernel throughput. Recheck the new candidate with alternating
baseline/candidate measurements and the correctness, overflow and pause gates;
this audit did not rerun that optimization or validate concurrent C17 changes.

For BSGS group 8, the compiler's 1,232 scratch bytes per lane and two estimated
waves/SIMD make private storage worth investigating. The kernel retains
`xs[8]`, `ys[8]`, `zs[8]` and batch-inversion intermediates. Compare live-range
reduction or separately measured group sizes, retaining full public-key parity
and infinity handling. The recorded mixed-group defect should be corrected
before attributing aggregate results to one of these kernels.

## Earlier finding disposition

- **A15 addressed for standalone checkpoint timing:** summaries now accumulate
  kernel, transfer, seed, executor verification, owner revalidation, overflow,
  preparation and command wall costs. Attempted and useful work remain distinct.
  A19 is the remaining dispatch-identity gap found here; supervisor-wide
  throughput accounting is still separate from the standalone command.
- **A12, A13 and A14 remain open in C16.** Permanent xpoint overflow shrinkage,
  the missing xpoint launch bound and capacity-sized candidate work are unchanged.
- **A16 and A18 remain open.** The supervisor and its pause-related authorization
  paths are byte-identical to C15. This audit did not repeat the earlier long
  supervisor pause reproductions or infer that passing standalone controls fixes
  them. See the [C15 audit](C15_AUDIT.md).
- **A17 remains open.** Per-grant process startup and queue sizing can dominate
  coordinated utilization. C16's standalone benchmark explicitly excludes that
  lifecycle and remote synchronization.

Reproduce the evidence checks and live mixed-group test after other GPU tests:

```sh
HIP_VISIBLE_DEVICES=0 python3 tools/audit_c16.py \
  --source /path/to/isolated/92486d8 \
  --build /path/to/isolated/hip-build \
  --report /tmp/C16_AUDIT_PROBES.json
```

Omit `--build` to check the retained evidence without running GPU work. The live
probe requires the C16 build dependencies and `rocprofv3`; it retains its temporary
benchmark journals and trace artifacts for inspection.
