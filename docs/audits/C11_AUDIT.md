# C07–C11 HIP correctness and performance audit

Audited revision: `4febd4a`, frozen on **2026-09-30**. C12 commit `0c21349` and
additional storage changes appeared during review; they are outside this audit.

**No new coverage failure was found in the tested HIP paths.** All 37 HIP tests,
22 CPU-only tests, and 22 additional runs of the earlier missed-target fixtures
passed. The new HIP implementations recover every target missed in the C06
audit. Three reproducible performance findings remain, including a two-line
compiler-hint experiment that improves large-batch xpoint kernel throughput by
**1.48–1.51×**.

C11 meets its tested search acceptance gate on the device below. These findings
do not block C12 development. They should be addressed before treating current
xpoint throughput as the tuned baseline. Production implementation files were
not changed; the experiment is supplied as a separate patch.

## Scope and evidence

- AMD Instinct MI300X, `gfx942`, SPX/NPS1, 304 CUs, wave64; runtime ordinal 0,
  PCI `0000:1b:00.0`. All GPU commands used `HIP_VISIBLE_DEVICES=0`.
- Release builds from an archived commit, HIP runtime/driver `71526333`, the
  installed ROCm core-10.0 Clang toolchain, and explicit `gfx942` compilation.
- Independent pinned libsecp256k1 oracle commit
  `0cdc758a56360bf58a851fe91085a327ec97685a`; the repository's oracle self-test
  validates the vendored file hashes and compares with its Python affine model.
- [Validation evidence](C11_AUDIT_VALIDATION.json): complete CTest summaries,
  binary/build fingerprints, compiler commands and resource remarks.
- [Probe and timing evidence](C11_AUDIT_PROBES.json): inputs, expected/observed
  matches, exact receipt checks, output hashes, hardware inventory and raw samples.
- [Reproduction runner](../../tools/audit_c11.py) and
  [isolated experiment patch](C11_XPOINT_LAUNCH_BOUNDS.patch).

This audit did not retest the other seven SPX devices, CPX/QPX configurations,
package scaling, persistence, or NVIDIA hardware. CUDA remains a later milestone.
Compiler resource estimates below are not measured occupancy. GPU clocks and
partition settings were not changed. No sanitizer build was rerun in this audit.

## A12 — Recover normal batch size after xpoint overflow

Priority: **P2, reproduced performance defect**.

In [xpoint_command.cpp](../../src/backend/xpoint_command.cpp), lines 91 and
117–127, `attempt_limit` is initialized once, reduced after overflow, and never
increased again. A dense prefix therefore determines launch size for the entire
remaining job, including regions with no matches. This is the documented current
policy, rather than a discrepancy between documentation and implementation.

Reproduction: search hexadecimal `[1,1001)` with targets `X(G)` and `X(2G)`,
`--batch-size 4096 --kernel stepped`:

| Candidate capacity | Overflow attempts | Total launches | Successful coverage |
| --- | ---: | ---: | ---: |
| 1 | 1 | **4,097** | 4,096 scalars, both matches |
| 2 | 0 | **1** | 4,096 scalars, both matches |

With capacity 1, the initial 4,096-step attempt overflows. Every subsequent
launch contains one scalar, even after both targets have been found. Three
alternating-order pairs reproduced the same launch counts. The result is correct;
the performance problem is the permanent collapse in useful work per launch.
Process startup materially affects this tiny search, so launch counts are the
primary comparison, not a claimed end-to-end speedup ratio.

Use bounded interval splitting or controlled batch-size recovery after successful
retries. Preserve an ordered queue of uncredited intervals, emit nothing from an
overflowed attempt, and advance coverage only after a complete verified result.
A retry must still make progress under an entirely dense target set. Merely
restoring the original size immediately after every overflow would not suffice.

Acceptance: retain dense capacity-1 replay tests and add a dense-prefix/sparse-tail
case that checks exact matches, exact coverage, and recovery to larger batches.
Extend that case across multiple original work units. The current CLI tests prove
replay correctness but do not place a useful bound on this sparse-tail launch cost.

## A13 — Declare the actual 128-thread xpoint launch bound

Priority: **P2, measured optimization**.

[xpoint.h](../../kernels/hip/xpoint.h), lines 40 and 62, declares both kernels
without a launch bound. Every dispatch in
[xpoint.hip](../../src/backend/hip/xpoint.hip), lines 143–156, uses 128 threads.
The C11 BSGS kernels already supply this information to the compiler.

An isolated variant adds only `__launch_bounds__(128)` to `xpoint_direct` and
the templated `xpoint_stepped` declaration. AMD documents this hint as a way to
relax register restrictions imposed by unnecessarily large supported block sizes.
[HIP language extensions](https://rocm.docs.amd.com/projects/HIP/en/docs-7.0.2/how-to/hip_cpp_language_extensions.html#launch-bounds)

The existing benchmark checks exact matches and coverage, warms each executor,
and alternates direct/stepped execution. The audit additionally alternated
baseline/variant process order over five pairs: **25 warm samples per kernel,
workload, and build**, each searching 1,048,576 scalars. Other audit GPU jobs were
finished before these timings began.

| Stepped workload | Baseline kernel ms | Variant kernel ms | Kernel throughput ratio | Baseline / variant executor wall ms |
| --- | ---: | ---: | ---: | ---: |
| One target, no matches | 0.669441 | 0.444089 | **1.507×** | 0.717883 / 0.491228 |
| Three boundary targets | 0.708370 | 0.478287 | **1.481×** | 0.767218 / 0.537748 |
| 32 targets, no matches | 1.867725 | 1.259302 | **1.483×** | 1.916538 / 1.308751 |

Values are medians of the 25 warm samples. Baseline and variant kernel-time
ranges do not overlap for any of these workloads. Executor wall time improves
by 1.43–1.46×. The direct reference kernel also improves by approximately
1.75–1.76×, while remaining substantially slower than stepping.

| Kernel | VGPRs, before → after | VGPR spills, before → after | Scratch bytes/lane, before → after | Estimated waves/SIMD, before → after |
| --- | ---: | ---: | ---: | ---: |
| Direct | 128 → 160 | 84 → 0 | 164 → 36 | 4 → 3 |
| Stepped, at most four targets | 128 → 180 | 220 → 0 | 260 → 0 | 4 → 2 |
| Stepped, larger target sets | 128 → 162 | 105 → 0 | 912 → 784 | 4 → 3 |

The result illustrates why maximizing the compiler's occupancy estimate alone is
insufficient. In these workloads, eliminating spills wins despite fewer estimated
resident waves. The larger-target kernel still has substantial local scratch.

The variant passed all four affected CTest suites: xpoint executor, failure
injection, stepped CLI, and direct CLI. The CLI suites contain 41 oracle cases
each, including tails, high scalar bits, overflow, and maximum local offsets.
The patch is ready for implementation review. Retain the 128-thread dispatch
contract and remeasure on other intended devices/partition modes and compilers;
the measured ratios apply to this MI300X configuration and large-batch workload.

## A14 — Bound output traffic by possible candidates

Priority: **P2, measured configuration-dependent overhead**.

[xpoint.hip](../../src/backend/hip/xpoint.hip), lines 42, 138 and 161, clears
and downloads the entire allocated candidate slot on every batch. Traffic depends
on the requested capacity even when there are zero candidates.

A no-match run with one target, four 1,048,576-scalar batches, and three alternating
capacity pairs produced:

| Requested capacity | Download bytes/batch | Warm download ms | Warm executor wall ms |
| --- | ---: | ---: | ---: |
| 1,024, the default | 16,424 | 0.022131 | 0.707486 |
| 1,048,576, the maximum | 16,777,256 | 0.319286 | 1.030205 |

Timing entries are medians across the three runs' post-first-batch medians. The
maximum slot increases warm executor wall time by about **46%** in this workload.
The default's small transfer is not evidence of a comparable default slowdown.

Before adding another host/device round trip to fetch the count, exploit exact
candidate bounds. For current unique full-X targets, a scalar interval inside
`[1,n)` contains at most two matching scalars per target. An effective capacity of
`min(requested_capacity, max_steps, 2 * unique_target_count)` preserves existing
overflow behavior while avoiding impossible output allocation. Preserve the guard
record, validation, and reported allocation accounting when changing the slot.

The same pattern exists in
[bsgs_search.hip](../../src/backend/hip/bsgs_search.hip), lines 47, 150 and 175.
With the current exact signed-point table and bounded tile, each target can yield
at most one candidate; a batch has at most 64 targets. The default capacity 1,024
and maximum 65,536 exceed that possible output. This BSGS extension is a
source-derived opportunity; its timing benefit was not measured here.

## Correctness results and earlier findings

The HIP suite passed in 314.63 seconds; CPU-only passed in 32.68 seconds. The
isolated launch-bound variant's four affected suites passed in 53.94 seconds.
These runs include explicit errors for unavailable backends and failure-injection
checks; successful CPU-only tests do not imply GPU fallback.

- GPU arithmetic: 13,381 field cases and 1,278 point cases, zero failures.
- Existing xpoint integration: 41 cases for each kernel, 29 rejection checks each.
- Existing BSGS integration: 88 cases for each of `auto`, `1`, and `8`, with
  37 rejection checks per mode. These include infinity, opposite signs, exact
  tails, all-target replay and maximum local giant-offset bits.
- Additional C06 regressions: four xpoint fixtures × two kernels × two capacities,
  plus the BSGS fixture × three group modes × two capacities: **22 passing runs**.
  BSGS used `m=1024` and the original `[0x100000,0x300000)` interval.
- Every extra regression checked exact expected match multiplicity, contiguous
  successful coverage, zero credit on overflow, target-subset progression for
  BSGS, and summary counter consistency.

| Earlier finding | Current disposition |
| --- | --- |
| A01: faulty external CUDA reference arithmetic | New HIP arithmetic is separate and passes its oracle checks. The external reference remains unsuitable for unchanged reuse; this is not CUDA-backend validation. |
| A02/A10: exceptional points and silent legacy misses | Addressed in the tested new HIP paths. All five historical fixtures now return every expected target. This does not establish a fix to the legacy CPU loops. |
| A11: narrow point-operation corpus | Partly addressed for HIP: its pool includes 12 random full-width points. Projective scales remain `1`, `7`, `p-1`; the older CPU pool selection is unchanged. Expand arbitrary random scales/pairs before further arithmetic tuning. |

## Next performance experiments

These are source-based opportunities, not measured improvements from this audit:

1. After A13, profile the remaining large-target xpoint and grouped BSGS scratch.
   Compare compile-time indexing/unrolling and smaller serial inversion groups,
   recording spill traffic and end-to-end search time. Avoid choosing a group size
   solely from its reduction in inversion count.
2. Benchmark a specialized field square and a fixed inversion addition chain.
   [field.h](../../kernels/common/field.h), lines 136–146, uses general multiplication
   for squaring and binary exponentiation for inversion. Keep zero and aliasing
   contracts and rerun the independent arithmetic and actual search tests.
3. Implement and compare a complete mixed-coordinate addition path.
   [point.h](../../kernels/common/point.h), lines 98–103, currently forwards mixed
   addition to the general formula. Cached affine increments can avoid redundant
   operations, but equal/opposite/infinity cases must remain correct.
4. Measure table preparation and cache validation at realistic table sizes.
   [bsgs_table.cpp](../../src/core/bsgs_table.cpp), lines 74 and 131, derives every
   `jG` separately during build and semantic validation. Consider bounded point
   stepping/batch normalization and a reusable validated host owner across jobs.
   Retain full semantic validation; replacing it with checksum-only acceptance
   would reopen the rehashed-corruption coverage risk.

## Reproduction

Build the baseline from archived commit `4febd4a`. Build a second isolated copy
after applying `C11_XPOINT_LAUNCH_BOUNDS.patch`; do not apply it to an active
implementation checkout merely to reproduce the audit. Both use Release,
`BUILD_TESTING=ON`, `KEYHUNT_ENABLE_HIP=ON`, and `CMAKE_HIP_ARCHITECTURES=gfx942`.

```sh
HIP_VISIBLE_DEVICES=0 ctest --test-dir /path/to/baseline-build --output-on-failure --parallel 1

HIP_VISIBLE_DEVICES=0 python3 tools/audit_c11.py \
  --binary /path/to/baseline-build/keyhunt \
  --oracle /path/to/baseline-build/secp256k1_oracle \
  --benchmark /path/to/baseline-build/hip_xpoint_benchmark \
  --variant-benchmark /path/to/variant-build/hip_xpoint_benchmark \
  --report /tmp/c11-audit-probes.json

HIP_VISIBLE_DEVICES=0 ctest --test-dir /path/to/variant-build \
  --output-on-failure --parallel 1 \
  -R '^(hip_xpoint_failures|hip_xpoint_executor|xpoint_cli|xpoint_cli_direct)$'
```

The original isolated builds and raw logs are under
`/tmp/keyhunt-c11-audit-veoz264f/`. The repository artifacts linked above preserve
the principal evidence without relying on temporary-directory retention.
