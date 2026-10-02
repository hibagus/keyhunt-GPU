# Native CUDA backend (C18)

C18 compiles the same bounded search algorithms and executor lifecycle with
NVIDIA nvcc. CUDA and HIP use separate build directories and native runtimes.
The H200 preset targets `sm_90` and embeds `compute_90` PTX for forward JIT
compatibility. No HIP SDK is required for CUDA or CPU builds. The later
[C20 H200 acceptance](C20_CUDA_VALIDATION.md) validates concurrent CUDA owners,
coordinator/HTTPS integration, 1/2/4/8-GPU scaling and calibrated recovery.

## Build and run

Requirements: CMake 3.22+, GCC/Clang for the existing x86 CPU verifier, an NVIDIA
CUDA Toolkit, and SQLite 3.51.3+ development files. This session uses CUDA
13.3.73, NVIDIA driver 610.57.04, GCC 11.4, and eight NVIDIA H200 GPUs reporting
compute capability 9.0. The toolkit was installed at `/usr/local/cuda` but its
`bin` directory was absent from `PATH`.

```sh
cmake --preset cuda-h200 -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
cmake --build --preset cuda-h200 --parallel 12
TMPDIR=/var/tmp ctest --preset cuda-h200 --parallel 8
build/cuda-h200/keyhunt devices --backend cuda
build/cuda-h200/keyhunt gpu-smoke --backend cuda --steps 257
```

Use the [usage guide](USAGE.md) for finite searches with explicit fixture setup
and durable checkpoints. If SQLite is outside the normal SDK paths, pass `SQLite3_INCLUDE_DIR` and
`SQLite3_LIBRARY` as in [BUILD.md](BUILD.md). Validation built SQLite 3.51.3 in
`/tmp/keyhunt-c18-deps` from the official amalgamation; no system packages were
changed. See [the temporary-directory finding](C18_TEST_ENVIRONMENT.md).

The existing `bsgs-table validate`, `bsgs`, and `checkpoint run` commands accept
`--backend cuda`. Table generation/inspection, state management, target encodings,
exclusive range endpoints, and checkpoint formats are unchanged. Use
`CUDA_VISIBLE_DEVICES` to restrict/reorder visible ordinals. Persisted device UUIDs
identify visible CUDA instances; a PCI BDF is not treated as a physical-package
identifier because MIG instances can share one. MIG itself has not been validated
on this host. Memory budgets use runtime free/total memory for the selected device.

`tools/benchmark_gpu.py` and `tools/coordinator_worker.py` accept `--backend cuda`;
HIP remains their default for existing invocations. The optional HTTPS worker
self-test uses the selected native build. Enable `KEYHUNT_ENABLE_COORDINATOR=ON` for workers; the [C20 acceptance](C20_CUDA_VALIDATION.md)
validates that optional configuration with localhost mTLS, eight concurrent
owners and recovery. Public ingress/cross-host coordination remains outside that gate.

## Shared implementation and failure contracts

Public `gpu_*.h` headers contain no SDK headers. Earlier `hip_*.h` names remain
source aliases for clients. `src/backend/gpu` owns bounded buffers, pinned staging,
streams, events, tickets, poison-on-failure, and CPU verification. `kernels/search`
contains shared search kernels. The private `kernels/device_runtime.h` adapter
maps allocation/copy/event calls to the native SDK; it contains no search policy.
CUDA discovery alone uses NVIDIA device properties. AMD discovery and its CPU
partition test retain their existing hardware semantics.

The exact C05/C11 plans, full candidate verification, guarded capacity, overflow
replay, C13 durable receipts and C14 controls are shared. A CUDA error never falls
through to a CPU search. Destruction drains only owned streams. CUDA does not
change clocks, reset devices, reserve all GPUs, or alter compute partitions.
C18 executes one selected device per owner. C20 adds the validated concurrent
multi-device scheduler described in [MULTI_GPU.md](MULTI_GPU.md).

Device selection also restores the caller's ordinal if scope construction throws
after the native runtime has changed it. The failure test injects both get-device
and post-set-device errors; with two visible GPUs it verifies an actual selection
change is undone. This protects shared CUDA and HIP executor construction.

The shared arithmetic/search/failure tests compile with either GPU language.
Native CUDA validation includes independent Python field arithmetic and pinned
libsecp256k1 point/search oracles, every visible device, exact tails and high-bit
ranges, output corruption and SDK failure boundaries, checkpoint kill/restart,
and graceful pause/resume with changed visibility.

## H200 tuning method

First retain the portable implementation as a measured baseline. Accept each
optimization separately only after correctness checks and repeated comparable
measurements. Kernel CUDA events measure device execution; executor and durable
benchmark wall times include different host costs and must remain separate.
Record registers and stack use with `cuobjdump --dump-resource-usage`, and inspect
SASS before interpreting an inline-PTX change. The public preset does not force a
register cap: spilled field temporaries can cost more than extra occupancy.

The [NVIDIA Hopper tuning guide](https://docs.nvidia.com/cuda/hopper-tuning-guide/)
describes SM register/occupancy constraints. The
[inline PTX guide](https://docs.nvidia.com/cuda/inline-ptx-assembly/index.html)
and [extended-precision ISA](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#extended-precision-integer-arithmetic-instructions)
define carry-chain semantics. Carry state must remain inside one assembly block;
compiler-visible inputs/outputs must describe every modified value.

Performance results apply to recorded workloads and this host. They are not a
claim that one kernel is globally optimal for every table size, target count,
launch size, or GPU. Later sections and `docs/baselines/C18_*` record accepted
optimizations and validation evidence.

## Selected-device initialization

CUDA 12 and later eagerly initialize the primary context in `cudaSetDevice`.
The initial port inherited full inventory discovery during every search startup,
which visited all eight H200s. `select_gpu()` now queries the visible count and
only describes the requested CUDA ordinal; `devices` still provides the full
inventory. Device scopes retain the previous thread selection. HIP discovery is
unchanged. See NVIDIA's [device initialization contract](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__DEVICE.html).

Five alternating 257-step diagnostic processes measured median startup plus
execution wall time of 3770 ms before and 756 ms after this change, with all eight
GPUs visible (unreserved host). This is a startup result, not a kernel speedup.
Raw observations are in [C18_DEVICE_SELECTION.json](baselines/C18_DEVICE_SELECTION.json).
The CLI checks cover hidden devices, invalid/unbuilt backends, `1,0` visibility
remapping and selected UUID identity.

CUDA PCI addresses are normalized to lowercase before the Linux sysfs lookup.
The runtime returned uppercase hex on this host, which previously made existing
NUMA metadata appear unavailable (`-1`). Discovery tests compare the reported
NUMA node with sysfs whenever that metadata is exposed.

## Fixed inversion chain

CUDA `inverse()` uses an explicit chain for `p-2 = 2^256 - 2^32 - 979`:
255 squarings and 15 multiplications, versus 256 and 249 for binary exponentiation.
Names `xK` denote `a^(2^K-1)` and the last four exponents are annotated in code.
Zero, in-place output and zero-containing batch inversion retain their contracts.
The merged C17 CPU/HIP path uses its short inversion chain. CUDA can use the
binary fallback with
`-DCMAKE_CUDA_FLAGS=-DKEYHUNT_CUDA_PORTABLE_INVERSE`.

The H200 microbenchmark median improved from 0.2550 to 0.1365 ms (1.87x).
At 32,768 xpoint scalars the 32-target stepped case improved 1.18x; one/three
small-target kernels do no inversion and remained unchanged. BSGS at `m=257`,
8,192 giants/target improved about 1.06–1.33x depending on grouping/target count.
All expected matches and coverage were checked, with one warm-up and five
samples per variant. Field and point oracle suites passed on GPU 7.

Applying the chain to direct xpoint raised registers from 92 to 124 and stack
from 64 to 224 bytes/thread, regressing that reference by about 19%. That dispatch
is rejected: direct xpoint explicitly retains `inverse_binary()`, restoring its
baseline timing. This preserves a distinct normalization reference for search
parity. [C18_INVERSION.json](baselines/C18_INVERSION.json) retains both the rejected
measurement and the corrected dispatch, together with independent oracle reports.

## Rejected PTX multiply experiment

A full-product PTX implementation added each 8x32 product row using separate
low/high `mad.cc`/`madc` carry chains. It kept all carries inside one asm statement
and delayed output writes until inputs were dead. Independent field/point oracles
passed, but complete searches regressed despite about an 8% isolated multiply
gain. SASS inspection showed that virtual PTX carry instructions still expand
into native multiply/add/condition operations; fewer PTX statements do not imply
fewer H200 cycles.

Leaving squaring in C++ improved the candidate, but did not recover the search
losses: stepped xpoint remained up to 8% slower and the 32-target, group-1 BSGS
case about 24% slower than the inversion-chain build. Both dispatches are rejected.
The retained default remains compiler-generated multiplication. The
[reproduction patch](audits/C18_PTX_MULTIPLY.patch) and
[raw measurements/resource evidence](baselines/C18_PTX_MULTIPLY.json) preserve
this finding without shipping a slower default or an unused experimental kernel.

## Accepted PTX carry chains

`kernels/cuda/carry.cuh` implements 256-bit add/subtract with a single local
carry chain, then exports a 0/1 carry or borrow. The shared code still performs
canonical field reduction and alias-safe publication. Inputs remain live until
all arithmetic finishes, avoiding an early output clobber if nvcc reuses registers.
The compiler is free to eliminate unused results; no volatile or memory barrier
is needed for this pure register operation. CPU/HIP retain the original C++ path.
`-DCMAKE_CUDA_FLAGS=-DKEYHUNT_CUDA_PORTABLE_CARRY` selects the CUDA fallback.

With the fixed inversion chain already enabled, median xpoint improvements were
1.34–1.35x for the direct reference and 1.13–1.16x for stepped kernels. BSGS cases
improved 1.08–1.16x; point doubling improved 1.20x. General multiplication and
inversion microbenchmarks were within about 1% of their previous timings, as
expected for this change. The independent field and point suites pass, including
carry boundaries, normalization, aliasing, zero, and exceptional curve points.
SASS contains native `IADD3.X` carry operations. Full raw samples, resource counts
and oracle reports are in [C18_PTX_CARRY.json](baselines/C18_PTX_CARRY.json).

## Mixed-coordinate stepping

CUDA search steps now use a complete mixed Jacobian/affine formula. The executor
has already verified and uploaded finite affine points with `Z=1` for the cached
powers and negative tile start, so the kernel can omit the second Z square and
four multiplications involving that Z. General points still use the complete
Jacobian path; doubling, inverse points, infinity and aliases remain explicit.
CPU/HIP retain the previous dispatch. Define `KEYHUNT_CUDA_PORTABLE_MIXED` in
CUDA compiler flags to reproduce the previous search path.

Against PTX carry chains alone, stepped xpoint improved 1.41–1.55x and the BSGS
cases 1.19–1.43x. Direct xpoint is unaffected. Independent point/field oracles pass;
matched boundary keys and exact coverage are checked in every benchmark sample.
The optional worker's production self-test source also compiled and passed its
xpoint and BSGS variants with the CUDA library; this did not start an HTTPS server.
[C18_MIXED.json](baselines/C18_MIXED.json) records timings, oracles and resources.

## Larger batches and durability

The final comparison uses the saved initial native port (`e980c3d`) and optimized
execution sources (`cc32365`), with identical workloads on physical H200 7. Each
variant has one warm-up and five checked samples. Xpoint uses 1,048,576 scalars;
BSGS uses `m=65537`, 32,768 giants per target, and automatic grouping in this table.
The host was unreserved, with a functional corpus running primarily on GPU 0.

| Search / targets | Initial CUDA median kernel ms | Optimized median kernel ms | Speedup |
| --- | ---: | ---: | ---: |
| Stepped xpoint / 1 | 1.994 | 0.969 | 2.06x |
| Stepped xpoint / 3 boundary matches | 1.991 | 0.922 | 2.16x |
| Stepped xpoint / 32 | 3.659 | 2.190 | 1.67x |
| BSGS auto / 1 | 0.540 | 0.314 | 1.72x |
| BSGS auto / 3 boundary matches | 0.586 | 0.333 | 1.76x |
| BSGS auto / 32 | 3.766 | 2.154 | 1.75x |

[C18_H200_LARGE_BATCH.json](baselines/C18_H200_LARGE_BATCH.json) contains raw samples,
binary hashes, direct-reference timings and explicit BSGS group 1/8 comparisons.
Automatic grouping is a heuristic: explicit group 8 is about 5% faster in the
one-target case above, while group 1 wins for smaller one-target batches. Profile
the intended geometry before overriding `--group-size`. The xpoint microbenchmark
uses synthetic no-match X values; the following CLI benchmark uses real points.

The pulled C16 harness passed all 108 processes (90 measured and 18 warm-ups):
both modes, one-target no-match, three boundary matches, and 32-target no-match,
each with volatile, timed, and every-batch checkpoints. Every run verifies exact
matches/coverage; durable variants also audit the resulting journal. The run uses
128 batches, up to 1,048,576 xpoint scalars or 32,768 BSGS giants per target per batch.
[C18_DURABILITY.json](baselines/C18_DURABILITY.json) preserves the full report,
compiler flags, device identity, process logs' hashes and concurrency notes.

Preparation dominates these short processes. For example, the one-target xpoint
case spends about 126 ms in kernels and 2.66 s in preparation, with median process
time 3.23 s. Committing each of 128 batches costs about 55 ms in checkpoint calls.
Process variation can exceed that difference; the samples do not establish that
one durability policy has lower steady-state overhead. Timed runs finish before
the 10-second interval, so they primarily measure the final commit. Kernel rates
exclude startup and storage; BSGS giant operations and equivalent scalar coverage
are separate counters. No multi-GPU throughput or long-duration stability claim
is inferred from these bounded runs.

```sh
CUDA_VISIBLE_DEVICES=7 TMPDIR=/var/tmp python3 tools/benchmark_gpu.py \
  --build-dir build/cuda-h200 --output-dir /var/tmp/keyhunt-cuda-benchmark \
  --backend cuda --repeats 5 --batches 128 \
  --batch-size 1048576 --giant-batch 32768 --target-batch 32 --m 65537
```

Choose a new output directory outside all checkouts. Runtime journals and target
files remain there; only measurement reports are committed under `docs/`.


## Final H200 acceptance

C18 is complete for native CUDA on the recorded H200/CUDA 13.3 stack.
[The validation report](baselines/C18_VALIDATION.json) retains test identities,
corpora, binary hashes, sanitizer output and device/recovery evidence.

- All 32 CPU tests pass. The CUDA build passes all 47 distinct tests: 39 from the
  initial run and eight completed after correcting the BSGS corpus timeout.
  The original 300-second timeout and interrupted superseded run are recorded;
  the new 600-second limit retains all cases and the 90-second child watchdog.
- Field and point suites pass 13,381 and 1,278 cases respectively. A further 129
  mixed arithmetic cases pass on each of eight H200s. Xpoint direct and stepped
  each pass 48 CLI cases; BSGS auto, group 1 and group 8 each pass 95 cases and 37
  rejection checks, including searches on every visible device.
- Candidate capacity, overflow/replay, output corruption, partial construction,
  queue errors and executor ownership checks pass. Checkpoint kill/restart,
  journal integrity, local controls, and pause/resume across 2/1/3 visible-device
  layouts pass. NUMA metadata agrees with Linux sysfs; selected UUID identity
  survives visibility remapping.
- NVIDIA Compute Sanitizer passes ten memory/leak runs and two uninitialized-read
  runs. The three lifecycle fixtures that deliberately select device `-1` disable
  API-error reporting for their memory checks; valid search benchmarks retain it.
  Initial expected invalid-device diagnostics are documented, not counted as
  memory defects. Failure-injection cleanup has zero reported leaks.
- The optional worker's production self-test passes against the final CUDA
  libraries. This validation does not start an HTTPS service or claim a full CUDA
  coordinator deployment. A fresh AMD HIP build and MIG validation were not
  available on this host; CPU HIP-discovery contract checks pass.

The final [resource report](baselines/C18_FINAL_RESOURCES.txt) records 122 registers
and no stack for small-target stepped xpoint, 110 registers/768 bytes of stack for
larger target sets, and 92 registers/1024 bytes of stack for grouped BSGS. Native
SASS retains `IADD3.X` carry instructions; its hash and reproduction command are
recorded in the validation report. These measurements leave room for further
workload-specific tuning; they do not establish a globally optimal kernel.

## C20 worker context isolation

The C20 selected-device path now has a native CUDA regression gate:
`coordinator_cuda_contexts`. It runs the production worker self-test on the last
visible ordinal in a fresh process, then queries every primary context with
[`cuDevicePrimaryCtxGetState`](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__PRIMARY__CTX.html).
The observer does not retain contexts or select devices. Only the chosen ordinal
may be active, catching both full-inventory initialization and accidental
restoration of the untouched default device. The gate explicitly skips when
fewer than two devices are visible; it cannot prove isolation on one GPU.
An empty visibility mask may return `CUDA_ERROR_NO_DEVICE` from `cuInit` before
count enumeration; the gate treats that as the same explicit skip, preserving
other driver initialization failures as errors.

On the eight-H200 host with CUDA 13.3 / driver 610.57.04, the fresh process
reported eight visible devices and exactly `[7]` active after all four worker
search variants passed. This validates the C20 fix for audit A20 on NVIDIA.
It does not simulate a driver hang or certify MIG behavior.

```sh
TMPDIR=/var/tmp ctest --test-dir build/cuda-c20 \
  -R coordinator_cuda_contexts --verbose
```

## Independent merged-revision audit

The [C17–C18 H200 audit](audits/C18_AUDIT.md) freshly builds `b765880` with NVCC
and passes 55 CUDA-build tests plus 12 Compute Sanitizer runs. It verifies C17's
shared compact-buffer, overflow-recovery and mixed-group accounting changes on
CUDA, measures warm and periodic-checkpoint throughput, and checks the A20 fix
separately at `fb57428`. Raw logs, source/binary fingerprints and independent
statistic checks are retained with that report. Full CUDA fleet acceptance is
recorded in the [C20 follow-up](C20_CUDA_VALIDATION.md); MIG remains unvalidated.


## BSGS tile traversal

Native `bsgs`, `checkpoint run` and supervised workers accept execution-only
`--tile-order forward|reverse|both-ends|dance|random-window`. Reverse selects the highest remaining scalar
tile within each grant, preserving all-target completion and actual scalar
receipts. Both-ends alternates low/high tiles and starts low again after restart;
see [its contract](C23_BSGS_BOTH_ENDS.md). Dance cycles low/high/middle with a
fixed midpoint and exact coverage; see [contract and example](C23_BSGS_DANCE.md).
Random-window shuffles ascending windows of up to `--tile-window 1..256` tiles
(default 64), using `--tile-seed HEX` (256-bit, default zero). These overrides
require random-window. A window fixes work ownership before shuffling and samples
new adaptive sizing only for the next window. Restart resets the stream over
saved gaps; seed/window may change. See [the contract and executable
example](C23_BSGS_RANDOM_WINDOW.md) and [HIP/H200 acceptance](C23_BSGS_RANDOM_WINDOW_VALIDATION.md).
Tile order can change on restart without
recreating the job. See the
[contract and executable HIP/CUDA example](C23_BSGS_REVERSE.md).


## Minikey ordinal orders

Native minikey search, checkpoint runs and workers accept `--ordinal-order
forward|reverse|both-ends|dance|random-window` for both lengths and all supported encodings. Reverse changes
batch selection and lane-to-ordinal mapping, preserving canonical ordinal
receipts. Both-ends alternates successful batches between low and high endpoints,
including inside adaptive work reservations. Direction is read from each
submission and can change after recovery. Dance cycles low/high/fixed-midpoint-forward
with a pivot that prevents unbounded interior fragmentation. It reuses existing
forward/reverse kernels; no device arithmetic changes are needed.
See [the contract/example](C23_MINIKEYS_REVERSE.md) and
[reverse acceptance](C23_MINIKEYS_REVERSE_VALIDATION.md), plus
[both-ends contracts](C23_MINIKEYS_BOTH_ENDS.md) and
[both-ends HIP/H200 acceptance](C23_MINIKEYS_BOTH_ENDS_VALIDATION.md).
The [dance contract](C23_MINIKEYS_DANCE.md) defines midpoint, overflow and restart
behavior; [dance acceptance](C23_MINIKEYS_DANCE_VALIDATION.md) records its checks.
Random-window shuffles ascending windows of 1..256 fixed ordinal tiles using
`--ordinal-window` (default 64) and `--ordinal-seed HEX` (256-bit, default zero).
The selected tile retains its unaccepted suffix through overflow. New windows
sample current batch/work sizing; restart resets the stream on saved gaps.
See [the contract/example](C23_MINIKEYS_RANDOM_WINDOW.md) and
[HIP/H200 acceptance](C23_MINIKEYS_RANDOM_WINDOW_VALIDATION.md).

## Scalar batch order

Scalar searches accept `--batch-order forward|both-ends|dance|random-window` (default forward).
This applies to xpoint, Bitcoin address/HASH160, Ethereum and vanity in native
searches, checkpoint runs, `keyhunt-worker run-device` and the Python supervisor.
Both-ends alternates low/high missing batches after acceptance, starting low.
Overflow retries the same phase and endpoint. Each batch preserves the immutable `--order`,
stride and orbit mapping; high batches are clipped at orbit variant boundaries.
Dance cycles low, high and the lowest missing endpoint at or above a fixed
midpoint, falling back to low when the upper half is exhausted. The midpoint is
computed once per invocation from the outer missing endpoints; batches and work
owners cannot cross it. Both-ends has at most two adaptive owners; dance has three.

Restart can switch batch order and geometry over the exact saved complement.
Dance recomputes its midpoint and starts low on restart.
Existing identities, receipts and capability requirements remain valid; compatible
older workers can continue in forward batch order. Creation, BSGS and minikeys
reject explicit `--batch-order` overrides, including forward. Mixed-mode workers
should omit this override. Execution summaries report `batch_order`.
See [contracts and an executable public example](C23_SCALAR_BOTH_ENDS.md) and
[both-ends HIP/H200 validation](C23_SCALAR_BOTH_ENDS_VALIDATION.md).
The [dance contract and public example](C23_SCALAR_DANCE.md) define the fixed
midpoint and recovery behavior; [HIP/H200 acceptance](C23_SCALAR_DANCE_VALIDATION.md)
records the checks and evidence.

Random-window shuffles up to `--batch-window 1..256` ascending canonical tiles
(default 64), using `--batch-seed HEX` (default zero). Both settings require
random-window. Tile boundaries stay fixed while overflow retries or orbit clipping
consume sub-batches; no new draw occurs until the selected tile is exhausted.
There are at most W tile records and W owners beyond the missing intervals.
Restart can change seed/window and resets the stream over the saved complement.
Execution records report `batch_seed` and `batch_window`. See the
[contract and executable example](C23_SCALAR_RANDOM_WINDOW.md) and
[HIP/H200 acceptance](C23_SCALAR_RANDOM_WINDOW_VALIDATION.md). None of these execution policies makes a performance claim.
