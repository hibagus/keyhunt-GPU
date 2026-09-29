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
