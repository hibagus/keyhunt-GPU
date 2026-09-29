# HIP backend foundation (C07)

C07 adds optional AMD HIP discovery and bounded diagnostic execution. It does not
implement a GPU search or mark any interval as searched. C08 supplies field/point
arithmetic; C09 supplies the first xpoint search.

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
CPU-only builds neither enable HIP nor probe its SDK. CUDA remains an explicit
configuration error (C18), as does the unvalidated HIP+sanitizers combination.

`devices --backend hip` prints JSON. Ordinals refer to the current process's
visible devices. Identity includes the raw HIP UUID bytes encoded as hex, PCI
BDF, architecture, compute units, lane width, driver/runtime versions, and
property/total/free memory bytes. `hipMemGetInfo` runs with each device selected;
its free count is a snapshot, not an allocation guarantee or table budget.
The previous thread-local device selection is restored.

Sysfs supplies package unique ID, partition modes and NUMA node when available.
Missing metadata is empty (NUMA: -1), with explicit warnings. Discovery on this
host reports **64 logical agents**. Eight agents expose package IDs and
CPX/NPS4/CAPPING metadata; 56 expose synthetic partition BDFs without PCI sysfs
nodes. AMD SMI also reports unavailable partition/package fields for these
siblings. We do not infer parents by clearing BDF bits. Complete physical mapping
and shared-memory contention remain prerequisites for C20 scaling claims.
Each logical agent currently reports 24 GiB HIP total memory, not full-card HBM.

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
Use one host thread per executor; this is not yet a multidevice scheduler.

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
A single slot intentionally provides backpressure until C09 introduces candidate
results. C13 will connect durable verification; C20 must isolate genuine device
hangs in supervised processes. A HIP call/destructor may block on broken hardware;
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
