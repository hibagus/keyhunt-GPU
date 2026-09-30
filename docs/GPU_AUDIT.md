# CUDA/HIP port audit — 2026-09-29

Latest follow-up: [C12–C13 audit and MI300X figures](audits/C13_AUDIT.md) checks
durable GPU execution and reports current warm and whole-process throughput.
The earlier [C07–C11 HIP audit](audits/C11_AUDIT.md) reproduces three performance
findings and measures a 1.48–1.51× xpoint kernel throughput improvement in an
isolated launch-bound experiment.

The earlier [C06 audit](audits/C06_AUDIT.md) covers arithmetic and silent misses
in the legacy CPU point-stepping loops. The C03 findings below remain a historical
record; the follow-up audits track their later disposition.

The design has useful correctness and recovery gates, but GPU performance is not
yet measurable: C03 is the last completed milestone in the audited revision,
`ad64ee864af4`. No device kernels are implemented there. **Do not import the
reference CUDA arithmetic unchanged.** This audit reproduces arithmetic failures
and identifies concrete performance experiments for the first HIP implementation.

Scope: the [GPU plan](GPU_REDESIGN_PLAN.md),
[coordinator plan](COORDINATOR_SERVER_PLAN.md), CPU hot paths/build configuration,
and `/home/bagus/keyhuntM1CPU` at `f80e95e`. Concurrent C04 working-tree changes
appeared during review; implementation conclusions and line numbers below refer
to the frozen C03 revision, not that ongoing work. Production code and existing
plans were not changed by this audit.

## Findings and disposition

P1 means a correctness/reuse gate that must close before the affected path can
credit coverage. P2 means a performance or implementation-design action to
measure and resolve at the indicated milestone. These are open findings; an
optimization proposal is not a measured speedup.

| ID | Priority | Finding | Gate |
| --- | --- | --- | --- |
| A01 | P1 | Reference multiplication and in-place doubling are incorrect; filter bit counts overflow at 512 MiB | C06–C10 |
| A02 | P1 | Optimized batch inversion needs explicit zero/exception handling and collision-safe exact verification | C08–C11 |
| A03 | P2 | Per-point inversion and large CPU temporary arrays are unsuitable starting points for a fast GPU kernel | C08–C11, C17 |
| A04 | P2 | Bloom/filter/exact-lookup costs need a GPU-specific comparison | C10–C11, C17 |
| A05 | P2 | Table sizing must account for construction peaks, reuse, target count, and partition contention | C07, C10, C20 |
| A06 | P2 | Initial profiling is scheduled too late; add measurements while interfaces are still flexible | C07–C11 |
| A07 | P2 | Submission needs explicit asynchronous buffer ownership and bounded host backpressure | C07, C09, C13 |
| A08 | P2 | Fixed spare inventory can starve faster GPUs between scheduled syncs | C15, C20 |
| A09 | P2 | The current generic CMake helper would propagate CPU flags if reused for GPU targets | C07, C18 |

## A01 — confirmed reference defects

The plan already flags arithmetic risks. A host probe now turns two of them into
reproducible failures. It compiles the reference function bodies unchanged with
CUDA qualifiers replaced by empty macros, then compares multiplication with
Python arbitrary-precision integers. This demonstrates C++ arithmetic defects;
it does not test CUDA/HIP compilation, scheduling, or hardware behavior.

- `cuda/secp256k1.cuh:204`: reduction uses a 32-bit limb times the 33-bit constant
  `0x1000003D1` inside a 64-bit sum. The carry can be lost. For
  `p = 2^256 - 2^32 - 977`, `(p-1)*(p-1) mod p` must be 1, but returns
  `fffffffefffffffefffffffefffffffefffffffefffffffefffffc2cfffff85f`.
  There were **29 failures among 81 edge combinations**; all 1,000 seeded random
  products passed. Random tests alone would have missed these failures.
- `cuda/secp256k1.cuh:308–316`: `pointDouble(&P, &P)` overwrites Y before computing
  Z from the original Y. For G, separate-output doubling produces the expected
  `Z = 2*Gy mod p`; the aliased call produces a different Z. `scalarMult` uses the
  broken aliased call at line 418.
- **Additional finding:** `cuda/bsgs_kernel.cu:59–61` computes `bloomSize * 8`
  using 32-bit unsigned arithmetic. A 512 MiB filter gives a zero modulus;
  larger sizes wrap. Its hashes and bit indices are also only 32 bits, so merely
  widening the size parameter cannot make all bits of a larger filter reachable.
  Specify a versioned 64-bit hash/address mapping and checked byte-to-bit sizing.
  This third finding is derived from source and integer widths, not a device run.

The plan's other reference exclusions still apply: ignored start offsets,
truncated match indices, silent candidate truncation, and missing CLI execution
wiring. The reference filter uses FNV-derived hashes, whereas the current CPU
filter uses XXH64; their native caches are not interchangeable.

Acceptance: differential field/point tests including maximal carries, aliasing,
zero, infinity and reduction boundaries; host sizing tests around 512 MiB and
4 GiB without allocating those buffers; actual device tests before reuse. Retain
the portable implementation as the comparison path for every ISA specialization.

## A02 — protect coverage while optimizing arithmetic and lookup

[`IntGroup::ModInv`](../src/crypto/secp256k1/IntGroup.cpp), lines 36–56, multiplies
every denominator into one product before inversion, with no zero handling in
that function. A zero denominator makes that product noninvertible. Copying this
routine into a cooperative GPU batch can invalidate other lanes in the batch,
not just the exceptional point.

In a new batch routine, replace zero denominators with the multiplicative identity
for the prefix/suffix calculation, retain a validity mask, and handle doubling,
inverse-point addition, and infinity through the correct point cases. Padded
inactive lanes also use the identity and emit no work. Every required participant
must reach collectives/barriers; an early return in a partial block can invalidate
the cooperative algorithm. HIP uses 64-bit lane masks even for wave32 devices;
hide the vendor differences behind tested lane adapters.
[HIP lane functions](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_cpp_language_extensions.html#warp-cross-lane-functions)

The CPU exact-lookup path is also unsuitable as the sole authority. At
[`src/app/keyhunt.cpp`](../src/app/keyhunt.cpp):3748–3772,
`bsgs_searchbinary` returns one index for a matching six-byte fingerprint. The
caller at 4330–4349 tries that index's two signs, without enumerating other
matching fingerprint entries; the shown verification compares X only. A fingerprint
collision can hide the correct entry. For a full-public-key BSGS target, final
verification must check `kG == Q`, including Y, and the assigned scalar bounds.

Acceptance: deliberate fingerprint collisions with the valid entry second/last,
both point signs, a target at the tile start, one zero among many valid inversion
inputs, all-zero and partial batches, and alias tests. Keep the C01 tests for
legacy characterization separate from these strict correctness tests.

## A03 — amortize inversions without spilling large arrays

The reference giant-step loop calls `toAffine` every iteration
(`cuda/bsgs_kernel.cu:159–165`). Its inversion performs **256 squarings plus 249
multiplications**, and `modSqr` currently calls the general multiplier. Those
505 multiplication-shaped operations precede point stepping and filtering.
It also computes affine Y even though this filter reads only X.

For B nonzero inputs, the prefix/suffix batch-inversion algorithm uses one
inversion and `3*(B-1)` field multiplications, excluding point generation,
normalization, storage, and synchronization. This is an operation-count argument
for an experiment, not a throughput claim.

The CPU implementation amortizes inversions, but its storage should not be
ported per GPU thread. At `src/app/keyhunt.cpp:2514–2520`, three 1,024-element
Point arrays occupy **360 KiB** with this ABI (`Point=120`, `Int=40` bytes), plus
20,520 bytes of denominator storage and another prefix array. Even a single
Point array is 120 KiB. Device representations should be smaller and independently
designed; per-thread arrays of this scale would create severe private-memory
pressure if retained.

Benchmark these alternatives on one logical device:

1. A simple correct Jacobian stepping kernel with a bounded normalization tile.
2. Affine stepping with batch inversions distributed across a wave or workgroup,
   using registers/LDS and explicit exceptional-point handling.
3. Small per-thread tiles or a split normalization kernel when collective overhead
   or register pressure makes the cooperative version slower.

Use a scalar multiplication for starting points, then repeated point additions;
the reference baby-table generator instead performs a 256-iteration scalar
multiplication per output (`cuda/bsgs_kernel.cu:198–215`). Evaluate batched starting
points and fixed-step table generation for C10 as well as the giant-step search.

For xpoint/BSGS lookup, normalize only X when possible, retaining the Y state
needed for subsequent stepping and exact verification. Evaluate specialized
squaring and reduction before assembly. Start with 8x32-bit limbs, but compare
complete kernels rather than assuming an ALU-width ratio predicts performance.

Sweep block sizes such as 64/128/256 and small points-per-thread values separately
for each backend. Record register use, scratch spills, LDS, useful work per second,
and pause latency. Do not optimize occupancy as an isolated score: both AMD and
NVIDIA describe the resource/latency tradeoff.
[AMD performance model](https://rocm.docs.amd.com/projects/HIP/en/latest/understand/performance_optimization.html),
[CUDA occupancy guidance](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#occupancy)

## A04 — choose the filter and exact table together

[`third_party/bloom/bloom.cpp`](../third_party/bloom/bloom.cpp):128–145 hashes each
query twice with XXH64, then computes a variable-width modulo per probe.
The configured `1e-6` error target implies about 28.76 bits/entry and up to 20
probes. **It does not imply 20 loads for every negative query:** this code exits
at the first missing bit. Measure actual probes, cache misses, and false positives.

Compare a conventional filter, a blocked filter whose probes stay within a small
region, and direct exact lookup for small target/table sizes. Compare fixed bucket
layouts or sorted fingerprint buckets with a GPU hash table; preserve every
collision entry. Sorting/compacting positives into a second kernel can reduce
divergence but adds traffic and launch cost. Use structure-of-arrays for bulk point
generation where lanes access the same limb; an aligned bucket layout may be
better for random lookup. There is no universal best layout for both operations.

Tune the measured quantity:

```text
cost/query = filter cost + false-positive probability * exact-lookup cost
             + candidate transfer/verification cost
```

A power-of-two bit count can replace division with masking only with a compatible
construction hash and a new format version. Parallel filter construction needs
atomic bit updates or a proven conflict-free build; the CPU byte read/OR/write
at lines 35–44 is not safe for concurrent GPU writers to the same byte. Verify
every inserted entry remains discoverable before publishing the table.

Acceptance: compare no-match and dense-match workloads at several table sizes,
include intentional collisions and a stressed parallel build, and record both
false-positive rate and total search time. Sending every Bloom positive to the
CPU may become the bottleneck; device exact lookup is an experiment worth making.

## A05 — size tables for peak live memory and actual topology

The plan already requires allocatable-memory and partition checks. Make C10's
budget include all allocations live during preparation and search:

```text
peak = entries + filters + sort/build scratch + any second table copy
       + target/point state + bounded output/stream buffers + runtime headroom
```

For illustration, a full table with `2^30` entries at 16 bytes/entry is 16 GiB.
A same-sized sort destination and a 28.76-bit/entry filter raise that to about
35.6 GiB before further scratch. A final table fitting a hypothetical 24 GiB
budget therefore does not mean its build fits. This example describes a proposed
full-table layout, not the legacy three-level table's memory formula.

Choose m by sweeping actual table formats, number of targets K, assigned span S,
and reuse horizon. A useful initial model is
`build_time(m) + K*ceil(S/m)*giant_step_time(m)`, amortizing build time across
compatible blocks. Lookup locality changes with m, so neither `sqrt(S)` nor
"use all free HBM" is an automatic optimum. Report cold-start and warm-table times.

For CPX/NPS4, compare one logical device, all selected sibling partitions, and
multiple packages. Logical partitions have topology and shared-resource effects;
peak physical-card bandwidth is not a per-agent performance expectation.
[AMD partition overview](https://instinct.docs.amd.com/projects/amdgpu-docs/en/latest/gpu-partitioning/mi300x/overview.html),
[AMD partition experiments](https://rocm.blogs.amd.com/software-tools-optimization/compute-memory-modes/README.html)

Start with explicit local allocations. Compare table replication against any
peer-access design using measured random-lookup latency, aggregate memory use,
and link traffic. Do not assume one table can be shared at local-memory speed.
If sharding tables later, every query must still reach the appropriate shard(s).
Independent blocks on different GPUs do not justify searching only a subset of
the required baby table. No partition, power, or clock settings were changed here.

## A06 — measure before C16

The milestone table puts the profiling harness at C16, after BSGS and recovery.
Keep that durable end-to-end milestone, but introduce a minimal harness at C07–C09:
event timing, host monotonic wall time, launch count, exact useful work count,
allocation/transfer volume, and compiler resource reports. Add arithmetic and
lookup microbenchmarks as C08/C10 land. Otherwise major representation decisions
will be made before their costs are visible.

Use an integer-arithmetic and random-memory performance model for these kernels;
floating-point or matrix-unit TFLOPS do not predict secp256k1 carry-chain throughput.
Profile representative kernels with the installed tool versions, and run primary
timing trials separately from expensive tracing/counter collection.

At C13/C16, extend the same records with CPU verification, durable commits,
replay, sync, and queue-empty time. Alternate baseline/candidate runs after warmup
and report spread plus median. Disclose other load on sibling partitions.
Archive portable/optimized build flags and code-object/resource evidence.

## A07 — asynchronous execution requires more than streams

The reference wrapper uses synchronous copies and a device-wide synchronize after
each launch (`cuda/bsgs_kernel.cu:277–310`). Reusing that interface would prevent
much of the overlap envisaged by the plan.

Allocate reusable device buffers, retain tables on the device, and use a small
bounded ring of result buffers. Each slot owns its completion event and generation
until copying and verification finish; reset its count only after ownership is
released. Use pinned host buffers for intended asynchronous transfers and verify
overlap on a timeline. HIP documents synchronous behavior for unpinned transfers.
[HIP memory API](https://rocm.docs.amd.com/projects/HIP/en/latest/reference/hip_runtime_api/modules/memory_management.html)

Coalesce local commits across devices while preserving per-batch result/coverage
ordering. Keep SQLite waits out of launch threads. Bound verification queues,
in-flight kernels, pinned allocations, and uncommitted output together; pause
latency includes draining all these stages, not just one kernel. More streams
cannot be assumed to help an already saturated kernel.

Overflow replay also needs a terminating strategy: reducing scalar span cannot
solve a single work item whose collision/target fan-out exceeds the output buffer.
At that limit, page the exact candidates/target groups, safely allocate sufficient
space, or return a visible resource failure with no credited coverage. A count
pass plus bounded emission is another option. Counters themselves must not wrap.
Test forced overflow, skewed collision buckets, and a verifier slower than all
devices combined. Add graphs or persistent kernels only if launch overhead remains
material after these simpler changes.

## A08 — expose the cost of the low-contact queue policy

`GPU_REDESIGN_PLAN.md:535–541` and
`COORDINATOR_SERVER_PLAN.md:308–315` cap spares at one per GPU and wait for scheduled
sync if inventory empties. This is an explicit operational choice, but it limits
throughput when a GPU completes immutable blocks much faster than the reference.

Let D be this device's block duration and T the sync interval. Even granting a
full active block plus a full spare just after sync gives at most `2*D` work.
If `2*D < T`, idle time is unavoidable under that cap. For hypothetical 36-minute
blocks and a two-hour sync, that is at most 72 minutes of work, or **60% utilization**.
This example is a 20x rate difference from the twelve-hour reference, not a
measurement of this host. Partial tails and fewer blocks than GPUs also cause
underutilization under the specified one-GPU-per-block rule.

Preserve strict scheduled mode, show predicted inventory exhaustion/idle time,
and offer the already planned early-refill option for throughput-focused operation.
If a larger inventory is later authorized, provision approximately
`ceil((T + margin)/D)` full blocks including active work, subject to quotas and
actual remaining durations. Validate mixed-speed and empty-queue cases, and
separate kernel-busy throughput from elapsed job throughput in reports.

## A09 — keep CPU compiler options on CPU compilation

[`cmake/CompilerOptions.cmake`](../cmake/CompilerOptions.cmake):10–23 applies
`-m64`, `-mssse3`, native tuning and optimized-build `-Ofast`/vectorization flags
without a C/CXX language restriction. This is appropriate to the preserved CPU
build but is an integration trap if `keyhunt_configure_target` is reused for HIP
or CUDA sources. It is not a current GPU build failure: those backends are disabled.

Create distinct host/device helpers or explicitly scope CPU flags by language.
Keep host crypto separate from portable device arithmetic. Inspect the first
gfx942 and CUDA compile commands; unsupported flags must not silently reach device
compilation. Test the requested backend independently from CPU-only builds, and
record compiler versions before drawing conclusions about representation or ISA.

## Review order and future mode work

Before approving C07–C11, close the P1 findings, add minimal profiling, implement
bounded point stepping/inversion, and compare filters/tables with peak-memory
accounting. Then measure the asynchronous path and scale across partitions. ISA
specialization follows a correct measured portable baseline as the plan requires.

C23 should use fixed-length hash kernels, share point generation across requested
encodings, and compare fused point/hash/lookup kernels with staged variants for
register pressure. Compressed and uncompressed SEC encodings have different
SHA-256 block counts; byte order, padding, Y parity and leading zeros require
known-vector checks. Ethereum needs its specified Keccak variant. Compile-time
mode specialization can remove unused work, but variants need the same coverage
and recovery tests. These are later experiments, not current implementation claims.

For each future optimization, retain the affected finding ID, exact revision,
independent correctness evidence, compiler resource report, raw paired timings,
device topology and pause/replay result. Reopen affected coverage if a defect is
found; CPU verification of emitted positives cannot detect silently missed matches.

## Validation and reproduction

- Frozen C03 snapshot: clean out-of-source Release configure/build succeeded;
  CTest passed, **38/38 CPU characterization cases**, including eight marked known
  defects. These results do not certify strict range coverage.
- Reference host probe: 1,081 products checked, 29 mismatches; in-place doubling
  disagrees with the expected Z. Evidence and exact source hashes are in
  [the probe JSON](audits/2026-09-29_REFERENCE_PROBE.json).
- [CPU validation summary](audits/2026-09-29_CPU_VALIDATION.json) records the binary
  and fixture hashes, commands and per-case outcomes. The isolated build and full
  logs were left at `/tmp/keyhunt-audit-je85m8dc` for local inspection.
- This audit ran no GPU search or GPU performance benchmark. `hipcc` and
  `rocprofv3` were found on PATH; `nvcc` was not. Hardware configuration is from
  the existing C01 capture, not a new allocation/topology certification.

Reproduce the reference probe from the repository root:

```sh
python3 tools/audit_gpu_reference.py \
  --reference /home/bagus/keyhuntM1CPU \
  --report /tmp/keyhunt-reference-probe.json
```

It deliberately returns exit status 1 when it detects arithmetic defects. It
needs g++ and Python, not a CUDA SDK or GPU. It is an audit aid for the external
prototype, not a replacement for C06's independently pinned oracle or device tests.
