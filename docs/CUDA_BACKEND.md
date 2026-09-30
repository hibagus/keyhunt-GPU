# Native CUDA backend (C18)

C18 compiles the same bounded search algorithms and executor lifecycle with
NVIDIA nvcc. CUDA and HIP use separate build directories and native runtimes.
The H200 preset targets `sm_90` and embeds `compute_90` PTX for forward JIT
compatibility. No HIP SDK is required for CUDA or CPU builds.

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
build/cuda-h200/keyhunt xpoint --backend cuda --range 1:10001 --targets targets.txt
```

If SQLite is outside the normal SDK paths, pass `SQLite3_INCLUDE_DIR` and
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
self-test uses the selected native build. CUDA coordination deployment is a
separate optional integration configuration, not implied by local GPU validation.

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
This milestone executes one selected device per owner; concurrent multi-device
scheduling remains C20.

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

## Fixed inversion chain

CUDA `inverse()` uses an explicit chain for `p-2 = 2^256 - 2^32 - 979`:
255 squarings and 15 multiplications, versus 256 and 249 for binary exponentiation.
Names `xK` denote `a^(2^K-1)` and the last four exponents are annotated in code.
Zero, in-place output and zero-containing batch inversion retain their contracts.
CPU and HIP retain binary inversion; CUDA can use that fallback with
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
