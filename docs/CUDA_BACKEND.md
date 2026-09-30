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
