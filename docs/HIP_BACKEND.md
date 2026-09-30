# HIP backend foundation (C07)

C07 adds optional AMD HIP discovery and bounded diagnostic execution. It does not
implement a GPU search or mark any interval as searched. C08 supplies
[portable field/point arithmetic](GPU_ARITHMETIC.md), and C09 implements the separate
[bounded xpoint search](HIP_XPOINT.md). The diagnostic commands retain their
transport-only meaning. C10 adds [immutable BSGS table uploads and lookup
validation](BSGS_TABLES.md), and C11 implements [bounded BSGS range searches](HIP_BSGS.md).

## Build and discovery

```sh
cmake --preset hip-release
cmake --build --preset hip-release --parallel 4
./build/hip-release/keyhunt devices --backend hip
```

The preset selects `gfx942`. On this host, CMake 3.22.1 discovers
`/opt/rocm/core-10.0/lib/llvm/bin/clang++` without overrides. HIP reports
7.15.26333; AMD clang reports 23.0.0git. No SDK installation or system changes
were needed. On other installations set `HIPCXX` to AMD clang++ and, if needed,
`CMAKE_HIP_COMPILER_ROCM_ROOT` to the SDK containing `lib/cmake/hip-lang`.
Do not use hipcc as CMake's HIP compiler. The backend uses CMake's HIP language;
legacy CPU flags and per-source LTO do not reach its translation units.
CPU-only builds neither enable HIP nor probe its SDK. Native CUDA is supported
in a separate [CUDA build](CUDA_BACKEND.md); enabling both backends together is
a configuration error, as is the unvalidated HIP+sanitizers combination.

`devices --backend hip` prints JSON. Ordinals refer to the current process's
visible devices. Identity includes the raw HIP UUID bytes encoded as hex, PCI
BDF, architecture, compute units, lane width, driver/runtime versions, and
property/total/free memory bytes. `hipMemGetInfo` runs with each device selected;
its free count is a snapshot, not an allocation guarantee or table budget.
The previous thread-local device selection is restored.

Sysfs supplies package unique ID, partition modes and NUMA node when available.
Missing metadata is empty (NUMA: -1), with explicit warnings. The original C07
CPX/NPS4 snapshot recorded 64 logical agents with 24 GiB HIP total memory each;
56 synthetic partition BDFs lacked PCI sysfs metadata. Those historical artifacts
remain unchanged. After the user switched the host to **SPX/NPS1**, fresh discovery
reports eight devices, 304 CUs each, and 206,141,652,992 HIP total bytes per device
(about 192 GiB). All eight now expose package and partition metadata.

Neither snapshot is an application constant. We do not infer parents by clearing
BDF bits or multiply a logical memory budget into a package budget. The [C20 fleet measurements](MULTI_GPU.md#measured-fleet-scaling) use eight
physical packages exposed as eight SPX devices; they do not measure partition
scaling or sibling-partition memory contention. See [partition compatibility](#cpx-qpx-and-spx-compatibility) for
the current hardware and simulated test coverage.

No visible devices yields an empty list. Runtime failures include the HIP
operation and named error and exit 2. CPU-only binaries reject the command with
an explicit build diagnostic; there is no CPU fallback. Invalid subcommand
options are rejected before touching the runtime.

## Discovery validation

Both CPU and HIP builds pass `backend_cli`: syntax errors, explicit unavailable
backend, JSON identity/memory invariants, unique logical UUIDs, and hidden devices.
The default CPU executable continues to run the preserved legacy CLI.
Discovery is read-only and never changes partition, clock or power settings.

API behavior was checked against the installed HIP headers and the official
[HIP runtime reference](https://rocm.docs.amd.com/projects/HIP/en/docs-7.14.0/doxygen/html/hip__runtime__api_8h.html),
[HIP memory reference](https://rocm.docs.amd.com/projects/HIP/en/docs-7.2.4/doxygen/html/group___memory.html),
and installed CMake 3.22 HIP compiler-detection module. Runtime/toolchain versions
above describe the tested host; they are not a promise of other-stack support.

## Bounded execution contract

```sh
./build/hip-release/keyhunt gpu-smoke --backend hip --device 0 --steps 257 \
  --start 0x100000000ffffffffffffffff
ctest --preset hip-release -L hardware
```

The diagnostic consumes C05 `KernelBatch` plans and returns a copy of the plan,
including assignment/executor generations and parent block identity. The kernel
adds a bounded local index to a full 256-bit start in big-endian byte form. The
host checks every returned scalar against `KernelBatch::scalar_at`, checks a
64-bit device execution counter, and checks a guard immediately after the tail.
No field or point arithmetic is involved. JSON explicitly sets
`diagnostic_only: true` and `search_coverage: false`; these results cannot
acknowledge searched coverage or matches.

The public header has no HIP types. Each executor owns one nonblocking stream,
three timing/completion events, reusable device buffers and pinned host buffers.
`submit` snapshots the plan and issues asynchronous work; `poll` queries the
completion event after the downloads. `take` verifies and copies the result,
then releases the slot. `drain` synchronizes only that executor's stream and
retains its unconsumed result. A second submission before `take` fails visibly.
Tickets include an executor identity and increasing sequence, so stale, foreign,
and double-consumed tickets are rejected. Result vectors survive later reuse.
Use one host thread per diagnostic executor. The search scheduler is described
separately in [MULTI_GPU.md](MULTI_GPU.md).

The configurable capacity is bounded to 1–1,048,576 indices (default 4,096).
The device and pinned buffers each use `(capacity + 1) * 32 + 8` bytes. Preparation
checks current free memory and preserves 64 MiB of headroom by default; actual
allocation errors still propagate. This small diagnostic budget is not a future
BSGS table-sizing policy. Preparation failures release partial allocations.
Destruction drains queued transfers before freeing their pinned buffers and
never resets the device. Explicit `drain`, `poll` and `take` report failures;
destructor cleanup is best effort and cannot throw. A runtime/verification error
poisons that executor; it yields no result and cannot accept a retry. Recreate it
after resolving the failure. No transparent fallback or automatic replay occurs.

A hardware test revealed that HIP's thread-local last-error state retains a
handled invalid-device error through later successful API calls. The launcher
checks setup calls directly, clears stale last-error status immediately before
launch, then checks this launch's status. The invalid-device-then-valid-launch
regression prevents a recurrence of this misleading failure.

Timings separate HIP kernel events, download events, host verification, and host
monotonic submit-to-take wall time. Wall time includes polling/consumer delay;
it excludes executor preparation. Launch count, device indices, allocation bytes
and download bytes are explicit. These atomically counted diagnostic indices are
not key-search throughput, and no transfer/kernel overlap is claimed. There is
no host-to-device bulk upload: the starting scalar is a kernel argument.
A single slot intentionally provides backpressure. C09 retains this ownership
model in its separate candidate-result executor. C13 connects durable verification;
C14 adds [local pause and resume controls](PAUSE_RESUME.md). [C20](MULTI_GPU.md) isolates device owners in supervised processes and tests
a stopped process, not a genuine driver hang. A HIP call/destructor may block on broken hardware;
run diagnostics under an external timeout in automation.

## Execution validation

`hip_executor` requires real hardware and exercises 1/255/256/257/511/1023/1024
indices, carry across 64 bits, the curve-order endpoint, nonzero block/cursor
identity, stream independence, retained results, repeated reuse, stale/foreign
handles, invalid devices, capacity/headroom rejection and destruction while work
is queued. Host verification also checks every index and tail guard on every run.
`backend_cli` exercises empty visibility, malformed/overflowing values and
unsupported options, CPU-only rejection, and a 257-index real launch.

`hip_failures` is a separate executable compiled with a test-only hook at 20
API boundaries: stream/event/device/pinned allocation, initialization, launch,
event recording, downloads, completion query, timing and draining. The real API
call runs before the injected error, so cleanup must account for partially owned
resources and queued operations. Failed executors reject reuse, and a fresh
executor verifies scalar 1 afterward. The production library contains no hook.
These are deterministic propagation/cleanup checks, not induced physical device
removal, actual out-of-memory exhaustion, or a claim that a hung driver recovers.

## Recorded C07 acceptance

[Validation results](baselines/C07_VALIDATION.json) record 14 passing HIP release
tests, 12 CPU release tests, 12 CPU debug tests, and the focused ASan/UBSan CLI
gate. They also retain configuration rejections, CPU/HIP dynamic dependencies,
installed-binary execution, logical-device 1 execution, and compiler resource
remarks (40 SGPRs, 17 VGPRs, zero scratch/spills for this diagnostic). These
compiler estimates are not a measured search-kernel performance result.

[Hardware and launch evidence](baselines/C07_HIP_DIAGNOSTICS.json) includes the
64-agent discovery snapshot, exact HIP compilation commands, binary digest,
seven bounded launches from scalar 1 through high-bit and order-boundary starts,
and the maximum 1,048,576-index capacity. Reproduce it with:

```sh
python3 tools/capture_hip_baseline.py --build-dir build/hip-release \
  --device 0 --report /tmp/keyhunt-hip-diagnostics.json
```

This harness uses separate short processes and records their event/wall times;
it is deliberately not a steady-state benchmark. C08 now adds
[arithmetic microbenchmarks](GPU_ARITHMETIC.md#initial-gfx942-measurements); C16
adds [durable end-to-end benchmarks and separate profiler capture](GPU_PROFILING.md).


## CPX, QPX and SPX compatibility

The HIP discovery and diagnostic executor support the devices exposed by all
three compute modes. No mode-specific build, flag, or kernel variant is needed.
The `gfx942` architecture remains the same when an MI300X changes partition mode.
Compute and memory partition strings are optional descriptive metadata; they
are not an execution whitelist or a formula for available memory.

- Enumerate `hipGetDeviceCount` on each discovery, using current process-local
  ordinals and UUIDs. Never assume 8, 32 or 64 devices.
- Read CU count, lane width and memory from each selected HIP device. Query
  `hipMemGetInfo` again when preparing an executor and retain allocation headroom.
  Do not assume 24/48/192 GiB, or independent memory pools for sibling partitions.
- Derive workgroup count from the bounded work item count. HIP schedules the
  groups across the selected partition; neither CU count nor XCC count changes
  the exact scalar interval or tail guard.
- Allow execution when secondary-partition sysfs metadata is unavailable. Keep
  missing metadata explicit instead of rejecting an otherwise valid HIP device.
- Restart workers and rediscover after repartitioning. Driver reconfiguration can
  invalidate handles and change ordinals/UUIDs; live switching inside an active
  executor is not supported. No test or application command changes partitions.

| Compute mode | Discovery contract tests | Real-hardware evidence |
| --- | --- | --- |
| CPX | NPS1 and NPS4; synthetic counts and per-device budgets | Original C07 CPX/NPS4 discovery of 64 agents, launches on ordinals 0 and 1 |
| QPX | NPS1 and NPS4; synthetic counts and per-device budgets | Pending a run on QPX hardware; simulated coverage does not certify device execution |
| SPX | NPS1; synthetic counts and per-device budgets | Current SPX/NPS1 host: all eight devices, 18 verified launches including reordered visibility |

The hardware/firmware determines which combinations are available. This host's
sysfs advertises SPX, DPX, QPX and CPX compute modes and NPS1, NPS2 and NPS4 memory
modes; those two lists do not imply every pairing is valid. AMD describes the
separate compute/memory dimensions, logical-device enumeration and QPX support
in its [partition reference](https://rocm.docs.amd.com/projects/amdsmi/en/docs-7.14.0/conceptual/partition.html).
The backend accepts the runtime's current devices instead of imposing its own
mode-pair matrix.

`hip_discovery_contract` compiles the actual discovery implementation with a
small test-only HIP API double and temporary sysfs tree. It runs without a GPU
SDK in CPU CI. Five profiles each check dynamic device counts, independently
reported property/total/free memory, missing secondary metadata, restricted and
reordered visibility, empty visibility, and restoration of the selected device
when a memory query fails. This exercises discovery, not simulated kernel math.

`hip_partitions` runs two bounded diagnostics on **every currently visible
device**, then checks ordinal remapping when no existing HIP/CUDA ordinal filter
would be overridden. It preserves the caller's ROCr visibility restriction. The
same test can run under CPX, QPX or SPX, with no expected device-count constant:

```sh
ctest --preset cpu-release -R hip_discovery_contract
ctest --preset hip-release -R 'hip_|backend_cli'
python3 tests/gpu/hip_partitions.py --binary build/hip-release/keyhunt \
  --report /tmp/keyhunt-current-partitions.json
```

[SPX validation](baselines/C07_SPX_NPS1_VALIDATION.json) records the five passing
focused tests, the sanitizer contract test, source fingerprints and build output.
[Raw SPX hardware results](baselines/C07_SPX_NPS1_HARDWARE.json) retain the inventory,
UUIDs, allocation/timing data, commands and remapping result. This extends the
original CPX acceptance and does not claim GPU search support or performance
comparisons between partition modes.
