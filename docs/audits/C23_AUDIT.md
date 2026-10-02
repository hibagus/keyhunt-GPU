# C23 CUDA/HIP audit — 2026-10-02

The audit found two new **P2 performance opportunities** in minikey search:
**A23**, compact admitted candidates before multiplication; and **A24**, use a
CUDA-specific launch bound. Packing gives much larger gains on H200 than MI300X;
GLV and launch-bound choices need separate decisions on each backend. The
isolated experiments and their limits are below. No new P1 correctness defect
was reproduced in the tested scope.
Production kernels, executor behavior and defaults are unchanged.

This report freezes `2a254bdba3f0c4d22401ca35387d320df1ad54cb`, including all
nineteen documented C23 acceptance slices. It covers HASH160/P2PKH, Ethereum,
vanity, minikeys 22/30, scalar strides/reverse/GLV/orbits, and the scalar, minikey
and BSGS batch traversal policies through seeded random windows. Earlier
acceptance documents describing C23 as partial describe their own phase;
[the current milestone table](../GPU_REDESIGN_PLAN.md) records the final scope.

## Fresh checks and their limits

| Build/gate | Result | Elapsed |
| --- | --- | --- |
| CPU release, complete CTest suite | 195/195 passed | 208.42 s |
| MI300X HIP release, selected CTest suite | 49/49 passed | 491.36 s |
| H200 CUDA release, selected CTest suite | 48/48 passed | 2447.40 s |
| Host ASan/UBSan, selected gates | 9/9 passed | 862.38 s |

The release builds include the coordinator and HTTPS worker. GPU selections
include all registered `hip_`/`cuda_` native arithmetic, encoding, executor and
fault gates, plus explicit random-window CLI, checkpoint/recovery, ownership,
HTTPS/offline and CUDA context regressions. The CPU build runs its entire suite.
The complete GPU-build CTest suites and every historical C23 recovery matrix
were **not** rerun; exact selections, commands, exit codes and JUnit output are
retained. Native device enumeration and selected tests cover the eight visible
devices, but performance below is for device 0 only. Transport gates use local
test coordinators on each host; this follow-up does not repeat C22's physical
cross-host file exchange for the new C23 modes.

Host sanitizers cover the four portable hash/GLV oracles, scalar mapping/planning
oracles and BSGS/minikey/scalar random-window storage recovery. They do not
instrument device kernels.

Three final H200 Compute Sanitizer memcheck runs cover the production minikey
executor test and the 44 correctness cases in each audit experiment; all report
zero errors. The first production invocation returned 97 with six expected CUDA
API diagnostics: the test deliberately constructs device `-1` once per length
and later clears each runtime error. Its original log is retained. A rerun uses
`--report-api-errors no`, leaving memory-access checking enabled; this is an
explicit qualification, not a claim that the unfiltered invocation passed.
[NVIDIA documents the API-reporting control separately from memcheck](https://docs.nvidia.com/compute-sanitizer/ComputeSanitizer/index.html#cuda-api-error-checking).
The experiments keep default API reporting. No full device racecheck/synccheck
matrix is claimed.

Each of the two minikey experiments passes **44 correctness cases and four
performance workloads per backend**. The first runs the unchanged baseline,
packed direct and packed GLV kernels; the launch-bound follow-up adds a bounded copy
of the original kernel and applies bounds to the two prototype stages. Cases
include both lengths and directions,
1/31/32/33/127/128/129/4,097-ordinal tails, no admitted candidate, the last 129
ordinals of the domain, both encodings and deliberately overflowing output.
Every run checks attempted work, count/overflow consistency and output/scratch
canaries. Non-overflow results must equal the full CPU relation set; overflowing
stored results must be unique members of that set. Overflow does not certify
coverage.

A separate Python check reconstructs admission using SHA-256, canonical targets
using affine secp256k1 mathematics and OpenSSL RIPEMD160, and exact logical
reverse offsets. It checks **8,208 distinct length/scalar combinations** per
report, including every admitted scalar in the measured million-ordinal ranges.
This checks the C++ benchmark's expected relations independently of its CPU ECC
implementation. The experiment is not a new production executor, recovery test
or implementation acceptance.

## A23 — pack admitted minikeys before expensive multiplication

**Priority: P2; measured experiment, not integrated.**

[`kernels/search/minikeys.h:24`](../../kernels/search/minikeys.h) calls
`public_key` inside the minikey check-byte branch. The
[admission function](../../kernels/common/minikey.h) rejects candidates whose
SHA-256 check byte is nonzero. The measured million-ordinal fixtures admit
4,096 and 4,110 candidates respectively. Expensive point multiplication thus
executes with many inactive lanes. This is a source-based performance hypothesis,
consistent with [AMD's description of wavefront divergence](https://rocm.docs.amd.com/projects/HIP/en/docs-6.3.1/understand/hardware_implementation.html)
and [NVIDIA's control-flow guidance](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#control-flow).
The timing experiment tests the proposed remedy; it does not isolate a hardware
counter attribution for the entire gain.

The comparison uses one executable with three variants:

1. The production `gpu::minikeys_direct` header kernel, unchanged, with 128 threads
   per block.
2. A 128-thread admission kernel compacting `(scalar, logical_offset)` records,
   followed by direct multiplication/encoding/exact lookup over the packed list.
3. The same packing stage followed by the existing portable GLV multiplier.

Both packed variants reuse the production serialization, hash and lookup body.
The second stage uses 256 blocks of 32 threads and reads the admitted count on
the device; there is no count download between kernels. This geometry is an
experiment parameter, not an established optimum for either architecture.

| GPU | Length | Ordinals | Baseline ms | Packed direct ms | Packed GLV ms | Packing gain | Packing + GLV gain |
| --- | --- | --- | --- | --- | --- | --- | --- |
| MI300X | 22 | 65,536 | 5.312 | 5.478 | 4.762 | 0.971× | 1.117× |
| MI300X | 22 | 1,048,576 | 9.671 | 5.701 | 4.977 | 1.696× | 1.939× |
| MI300X | 30 | 65,536 | 5.388 | 5.483 | 4.769 | 0.983× | 1.130× |
| MI300X | 30 | 1,048,576 | 9.632 | 5.760 | 5.038 | 1.674× | 1.915× |
| H200 | 22 | 65,536 | 5.798 | 2.983 | 4.028 | 1.945× | 1.450× |
| H200 | 22 | 1,048,576 | 41.439 | 3.637 | 4.941 | 11.400× | 8.385× |
| H200 | 30 | 65,536 | 6.129 | 2.984 | 4.579 | 2.058× | 1.340× |
| H200 | 30 | 1,048,576 | 41.917 | 3.656 | 4.957 | 11.491× | 8.449× |

Values are median kernel-event milliseconds over nine measured rounds after
two warmup rounds. Variant order rotates each round; gains are the median of
nine **paired** baseline/variant ratios, not a ratio of unrelated runs. The
packed timing includes both kernels. Four canonical targets select the first
and last admitted minikey in both encodings; candidate capacity is 16. No
measured samples were discarded. All raw rounds and per-pair ranges are retained.

At 1,048,576 ordinals, MI300X gains **1.674–1.696×** from packing alone and
**1.915–1.939×** with packed GLV. At 65,536 ordinals, packing alone loses about
2–3%; packed GLV gains 1.117–1.130×. H200 gains **11.400–11.491×** from packing
with direct multiplication on the large cases and 1.945–2.058× on the smaller
cases. Packed GLV is slower than packed direct on H200. The large H200 result
falls from about 41.4–41.9 ms to 3.64–3.66 ms; this is specific to the recorded
minikey kernels and fixtures. Retain different arithmetic choices per backend.
The separate launch-bound follow-up below measures another MI300X variant.

The recorded host `wall_ms` includes resets, launches, completion and result/count
downloads, but excludes setup, allocations and CPU verification. It is not the
production executor's submit-to-take metric. These numbers are neither CLI nor
durable/coordinated throughput. Ordinals include check-byte rejections and must
not be reported as valid private keys per second.

The experiment allocates `40 * (count + 1)` scratch bytes: **41,943,080 bytes** at
1,048,576 ordinals, including a guard. Allocation is bounded by the worst-case
candidate count, never by the observed admission probability. Integrating this
would require adding scratch to selected-device memory/headroom accounting,
handling partial allocation and launch failures, and preserving poisoning,
ticket ownership and teardown. Coverage must wait for both kernels and exact
verification; output overflow must still reject the whole attempt and replay
its original ordinal mapping. No production scratch path is added here.

Recommended next change: make packing an explicit experimental minikey strategy,
retain direct fallback for small batches, benchmark more sizes and target
counts on both architectures, and pass the existing allocation/failure,
overflow/restart and online/offline owner gates before considering a default.
A batch-size threshold cannot be inferred from two sample sizes.

## A24 — use backend-specific minikey launch bounds

**Priority: P2 for CUDA; reject the same baseline change on HIP.**

The [production minikey kernel](../../kernels/search/minikeys.h) has no explicit
launch bound, while [its executor](../../src/backend/gpu/minikeys.cpp) always
launches 128 threads. An audit-only copy changes just the kernel annotation to
`__launch_bounds__(128)`. A separate four-variant executable rotates the original,
bounded original, bounded packed direct and bounded packed GLV kernels over the
same public fixtures, correctness gates and two-warmup/nine-measured-round design.

| GPU | Length | Ordinals | Original ms | 128-thread bound ms | Paired speedup |
| --- | --- | --- | --- | --- | --- |
| MI300X | 22 | 1,048,576 | 9.690 | 10.970 | 0.881× |
| MI300X | 30 | 1,048,576 | 9.730 | 11.357 | 0.865× |
| H200 | 22 | 1,048,576 | 41.575 | 34.860 | 1.197× |
| H200 | 30 | 1,048,576 | 41.890 | 35.066 | 1.198× |

The CUDA-only annotation is a smaller potential change than packing: no new
scratch buffer, mapping or output semantics. It gives **1.197–1.198×** paired
kernel speedup on the two large H200 workloads. Every large-workload pair
improves; 65,536-ordinal cases are close to parity with about a 1% median penalty.
The corresponding large MI300X cases instead take about **13–16% longer** by
paired ratios. Do not infer a portable annotation policy from the earlier
xpoint tuning result. Before integrating the CUDA-only change, repeat the
production executor/failure/recovery gates and measure a broader target/batch
matrix with the actual production binary.

Compiler metadata is retained as supporting context, not measured occupancy.
On gfx942, the original kernel uses 128 VGPRs and 372 bytes of private segment;
the bounded original uses 133 VGPRs and 356 bytes. On H200 the register counts
change from 100 to 94, with a 768-byte stack in both. These resource changes
alone do not explain or certify performance.

The follow-up also gives the admission prototype a 128-thread bound and its
packed curve stage a 32-thread bound. On MI300X the packed GLV variant then
measures **2.151–2.172×** against the unchanged baseline in that same executable
at 1,048,576 ordinals. Its compiler metadata changes from 128 VGPRs/636 private
bytes in the first experiment to 217 VGPRs/220 private bytes in the follow-up.
This combined follow-up is not an isolated paired estimate of the curve-stage
annotation alone. On H200, bounded packed direct still wins over bounded packed
GLV and measures about **11.41×** versus its unchanged baseline. Preserve the
backend distinction when developing A23.

## GLV remains workload dependent

Fresh runs also repeat the existing twenty-workload benchmark: four scalar
families, five scalar regions, 65,536 candidates, two boundary targets per
encoding, two warmup rounds and nine measured rounds per kernel. Each backend
retains 540 measured samples. The audit recomputes medians, ratios and sample
match counts from the raw records.

| GPU | Scalar region | GLV versus direct | GLV versus stepped |
| --- | --- | --- | --- |
| MI300X | `low` | 1.00–1.03× | 0.36–1.84× |
| MI300X | `bit128` | 0.57–0.64× | 0.07–0.65× |
| MI300X | `bit192` | 0.71–0.76× | 0.07–0.61× |
| MI300X | `dense256` | 1.39–1.47× | 0.07–0.64× |
| MI300X | `order` | 4.50–8.24× | 0.35–1.86× |
| H200 | `low` | 1.02–1.04× | 0.12–0.51× |
| H200 | `bit128` | 0.51–0.54× | 0.03–0.12× |
| H200 | `bit192` | 0.59–0.65× | 0.03–0.12× |
| H200 | `dense256` | 1.34–1.40× | 0.03–0.13× |
| H200 | `order` | 6.46–7.15× | 0.13–0.52× |

Here each range spans the four families and divides the reference median
kernel time by GLV's median; a value below 1 means GLV loses. All five regions
and host timing are retained in the JSON, including cases favoring GLV near the
curve order. Keep GLV opt-in. Its success in the packed minikey experiment does
not justify replacing stepped multiplication for contiguous scalar searches.

The MI300X audit build uses the default `KEYHUNT_GFX942_CARRY=OFF`; original C23
acceptance used a different recorded build configuration. These are fresh
same-binary kernel comparisons, not before/after revision regression claims.
No fleet scaling, clocks/power tuning or durable-throughput result is claimed.

## Reviewed correctness boundaries

- HASH160 binds compression encoding into canonical targets and verifies exact
  relations. Ethereum uses Keccak's domain suffix and the 64-byte X/Y payload;
  its independent oracle remains separate from production hashing. Vanity keeps
  overlapping prefix relations and bounds output by enabled prefix lengths.
- Stride/reverse mapping uses candidate coordinates with checked endpoints;
  orbit expansion is explicit and clips batches at variant boundaries. GLV
  changes multiplication, not coverage identity or the searched range.
- Random windows freeze bounded tile geometry, use domain-separated seeded
  shuffles and keep an overflowing tile's unaccepted suffix selected. Planner
  reservation does not credit coverage; accepted receipts consume exact gaps.
- The new owners reuse exact CPU verification, atomic result/coverage commits,
  capability fencing, cached-retry authorization and the existing failure
  lifecycle. The fresh regressions and historical evidence support these
  conclusions within their recorded workloads; they are not proofs for every
  input or deployment.

The earlier A22 BSGS launch-geometry finding remains separate. C23 traversal
policies do not themselves resolve that measured tuning opportunity.

## Evidence and reproduction

[Validation manifest](C23_AUDIT_VALIDATION.json),
[historical/fresh evidence checks](C23_AUDIT_EVIDENCE.json),
[minikey samples and independent checks](C23_AUDIT_MINIKEY.json),
[launch-bound comparison](C23_AUDIT_BOUNDS.json),
[fresh GLV measurements](C23_AUDIT_GLV.json), and
[raw logs/source/commands](C23_AUDIT_RAW_LOGS.tar.gz) accompany this report.
The manifest binds artifact/member hashes, build settings and binary hashes.
The raw archive excludes binaries, enrollment credentials and journal databases.

All **19** retained C23 validation manifests report success. The audit verifies
**96 artifact hashes**, **635 explicitly hashed raw members**, and **241 source
fingerprints**. Of those source fingerprints, 235 match the named last-production
revision and six test/CI fingerprints match later recorded commits. Those six
are retained as phase qualifications; they are not evidence that production
came from the wrong commit. Historical checks establish consistency with the
recorded evidence, not independent hardware execution of every historical case.
The original twenty-workload GLV statistics on both backends are recomputed too.

Fresh builds use separate frozen checkouts under
`/tmp/keyhunt-c23-audit-d5dvjeg4` on the MI300X host and
`/var/tmp/keyhunt-c23-audit-8nh9b_zi` on the H200 host. Exact configure/link/test
commands are in the archive. `TMPDIR=/var/tmp` keeps integration journals outside
the H200 host's unrelated `/tmp/.git` marker. The independent Ethereum oracle
uses pinned PyCryptodome 3.23.0 under
`/var/tmp/keyhunt-c23-ethereum-oracle`; it is not linked into production.

To recheck retained evidence against a checkout at the audited revision:

```sh
python3 tools/audit_c23.py --source /path/to/frozen-checkout \
  --glv /path/to/glv-hip.json --glv /path/to/glv-cuda.json \
  --output /tmp/c23-evidence.json
python3 tools/audit_c23_minikey.py --source /path/to/frozen-checkout \
  --input /path/to/minikey-hip.json --output /tmp/c23-minikey-check.json
```

Repeat the second command for the CUDA report. Raw input files, the audit-only
`minikey_probe.hip`, its generator and exact build commands are in the archive.
Its baseline includes the frozen production header. Run benchmarks after GPU
tests finish on an otherwise idle selected device; do not mix correctness-test
GPU load into the paired timings. The exploratory MI300X run is retained
separately from the final experiment and is not pooled into its statistics.
