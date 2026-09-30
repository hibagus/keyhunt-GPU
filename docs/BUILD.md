# Build and test

The preserved CPU engine requires Linux x86-64 and SSSE3 for its assembly and
hashes. Optional HIP discovery, diagnostics, bounded xpoint and BSGS searches,
and table preparation/lookup are implemented. `KEYHUNT_ENABLE_CUDA` builds the
native NVIDIA backend; see [the C18 guide](CUDA_BACKEND.md). CPU builds never
probe or download either GPU SDK.

Use CMake 3.22+, GCC/G++ (11.4.0 tested), Make, SQLite 3.51.3+ development
headers/library, and Python 3.9+ for tests. A
production-only build can omit Python with `-DBUILD_TESTING=OFF`. No dependencies
are fetched by the build. Clang is accepted but is not yet validated here.

```sh
cmake --preset cpu-release
cmake --build --preset cpu-release --parallel 4
ctest --preset cpu-release
./build/cpu-release/keyhunt -h
```

If SQLite discovery fails or finds an older version, supply `SQLite3_INCLUDE_DIR`
and `SQLite3_LIBRARY` as described in the [storage setup](STORAGE.md#database-and-deployment-boundary).
Use these same arguments when configuring each preset. The build never installs
or downloads SQLite. A custom library prefix also needs to be available to the
runtime loader after installation (for example through `CMAKE_INSTALL_RPATH` or
the deployment's library path); the default system development package avoids
that custom-prefix requirement.

The existing `-h` command returns status 1 after displaying help.
`cpu-debug` and `cpu-sanitizers` provide separate build directories. Sanitizers
currently expose known problems in the original engine; that preset is a
**diagnostic build, not a passing correctness gate**. See
[the migration analysis](BUILD_MIGRATION.md#validation-and-observed-defects).

```sh
cmake --preset cpu-debug
cmake --build --preset cpu-debug --parallel 4
ctest --preset cpu-debug
cmake --preset cpu-sanitizers
cmake --build --preset cpu-sanitizers --parallel 4
```

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `KEYHUNT_NATIVE_CPU` | ON | Use `-march=native -mtune=native`; turn OFF when the executable must avoid build-host-specific tuning |
| `KEYHUNT_ENABLE_LTO` | ON | Preserve the original selected translation units' LTO in optimized builds, when compiler support is available |
| `KEYHUNT_ENABLE_SANITIZERS` | OFF | Address/undefined sanitizers; disables LTO and additional `-Ofast` flags |
| `KEYHUNT_BUILD_LEGACY` | OFF | Separate GMP/OpenSSL executable |
| `KEYHUNT_BUILD_BSGSD` | OFF | Original local BSGS daemon |
| `BUILD_TESTING` | ON | Python/CTest regression checks |
| `KEYHUNT_ENABLE_HIP` | OFF | AMD HIP discovery, diagnostics, xpoint/BSGS searches and tables; requires ROCm AMD clang/runtime |
| `KEYHUNT_ENABLE_CUDA` | OFF | Native NVIDIA discovery, xpoint, BSGS and checkpoints |
| `KEYHUNT_ENABLE_COORDINATOR` | OFF | Registry, coordinator and durable HTTPS worker support; needs OpenSSL 3 and nlohmann JSON >=3.10 |
| `KEYHUNT_ENABLE_HTTPS_WORKER` | ON | With coordination enabled, build the libcurl HTTPS worker and Python supervisor; disable for server-only builds |
| `KEYHUNT_TEST_APACHE_ROOT` | Empty | Enable real Apache/mTLS integration tests using `/` or an extracted package root |

Changing tuning does not make the existing x86 engine portable to ARM. Release
uses C++17, GNU extensions, SSSE3, and the original `-Ofast`/vectorization flags.
Whole-target IPO is deliberately not enabled: it stalled the main program on the
validated stack. Do not set `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` for this code.
Use presets or an out-of-source `cmake -S . -B build/custom` configuration.

## Optional executables

The legacy target needs GMP and OpenSSL development packages (`libgmp-dev` and
`libssl-dev` on Debian/Ubuntu). They are required only when legacy is enabled.
Missing dependencies stop configuration with a diagnostic.

```sh
cmake -S . -B build/compatibility -DCMAKE_BUILD_TYPE=Release \
  -DKEYHUNT_BUILD_LEGACY=ON -DKEYHUNT_BUILD_BSGSD=ON
cmake --build build/compatibility --parallel 4
ctest --test-dir build/compatibility --output-on-failure
./build/compatibility/keyhunt-legacy -h
./build/compatibility/bsgsd -h
```

The CMake executables have distinct names, so building legacy cannot overwrite
the main executable. Custom dependency prefixes can supply `GMP_INCLUDE_DIR`
and `GMP_LIBRARY`; [C02 evidence](BUILD_MIGRATION.md) describes an isolated
validation setup without a system package installation.

The daemon test binds a per-process `127.77.x.y` loopback address on port 8080,
sends three synthetic requests, and always terminates the daemon. The original
`bsgsd` ignores its `-p` value when binding; this known defect remains unchanged
in the mechanical migration. Do not expose this old unauthenticated daemon as
the planned coordinator. See [BSGSD.md](../BSGSD.md) for its existing protocol.

## Make compatibility and local installation

`make` builds and copies the CPU binary to `./keyhunt`; `make bsgsd` copies the
daemon to `./bsgsd`. `make legacy` preserves its original convention of placing
the legacy binary at `./keyhunt`. Running `make` again restores the CPU binary.
Use CMake's separate executable names when working with both variants.

`JOBS=4`, `BUILD_DIR=build/custom`, and `CMAKE_ARGS='...'` customize the wrapper.
`make test` runs the CPU suite; `make clean` cleans the selected build targets
and root executable copies, leaving search results/caches alone.

```sh
cmake --install build/cpu-release --prefix /tmp/keyhunt-install
/tmp/keyhunt-install/bin/keyhunt -h
```

A local installation also carries the root and retained dependency notices.
Consult [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) before preparing
redistributable packages; this target is not a claim that the executable is
MIT-only.

## Core regression checks

C04 adds `result_verifier` and `target_loading` to CTest. Debug builds enable
libstdc++ bounds assertions. See the [core extraction notes](CORE_EXTRACTION.md)
for the original test cases. C06 corrected the inherited inversion overflow; the
focused verifier sanitizer test now passes. Whole-application sanitizer findings
remain open. See [oracle validation](ARITHMETIC_ORACLE.md).

C05 adds `exact_range`, `block_grid`, `work_unit` and `range_oracle`. The
[exact range notes](EXACT_RANGES.md) describe their exhaustive and wide-integer
checks. They run in the normal CPU suite and use no GPU SDK. The isolated planning
library also has a passing sanitizer gate:

```sh
cmake --preset cpu-sanitizers
cmake --build --preset cpu-sanitizers --parallel 4 \
  --target exact_range_test block_grid_test work_unit_test range_probe
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset cpu-sanitizers -L ranges
```

This selector excludes the existing CPU curve engine's known sanitizer failures.

C06 adds `oracle_selftest`, `cpu_field_oracle`, `cpu_point_oracle` and
`bounded_search_oracle`. The pinned test-only libsecp256k1 source builds offline
with upstream CMake and is excluded with `BUILD_TESTING=OFF`. See
[oracle provenance and reproduction](ARITHMETIC_ORACLE.md) for the source pin,
validation counts, arithmetic corrections and BSGS start-boundary change.
The current passing sanitizer selector is
`ctest --preset cpu-sanitizers -E 'cpu_baseline|target_loading'` with the sanitizer
environment shown above; the excluded whole-application checks retain known
findings. These focused gates do not certify the entire legacy application.

## HIP diagnostics

The tested MI300X preset uses native CMake HIP language compilation with
`CMAKE_HIP_ARCHITECTURES=gfx942`. CMake 3.22.1 discovers ROCm Core 10.0's AMD
clang 23.0.0git / HIP 7.15.26333 on this host without compiler overrides.

```sh
cmake --preset hip-release
cmake --build --preset hip-release --parallel 4
ctest --preset hip-release
./build/hip-release/keyhunt devices --backend hip
./build/hip-release/keyhunt gpu-smoke --backend hip --device 0 --steps 257
```

The full HIP suite includes the preserved CPU regressions and requires an
accessible GPU. `ctest --preset hip-release -L hardware` selects the HIP
hardware checks; missing hardware fails those gates. The backend's device code
receives no CPU SIMD/native, fast-math or LTO flags. HIP+sanitizers is explicitly
rejected; use the separate CPU sanitizer build for host checks. Installation
includes the same `keyhunt` executable and uses the installed ROCm runtime.
See [HIP_BACKEND.md](HIP_BACKEND.md) for alternate SDK paths, partition identity
limitations, memory/ownership rules, and reproduction of the recorded evidence.


## Portable GPU arithmetic checks

C08 builds `portable_arithmetic_probe` in CPU test configurations and adds
`hip_arithmetic_probe` and an opt-in `hip_arithmetic_benchmark` to HIP test builds.
Shared integer headers compile without CPU native/SIMD, LTO or fast-math flags.
The public C07 diagnostic commands retain their transport-only behavior.

```sh
ctest --preset hip-release -L arithmetic
ctest --preset cpu-release -L arithmetic
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset cpu-sanitizers -L arithmetic
./build/hip-release/hip_arithmetic_benchmark 0
```

The arithmetic label includes field and point differential tests and, on HIP,
a small corpus on each visible logical device. Benchmark output is JSON with
warm-up, raw kernel-event samples and checked results; timing never decides a
CTest pass. See [GPU_ARITHMETIC.md](GPU_ARITHMETIC.md) for exact representation,
zero/infinity/aliasing contracts, benchmark interpretation and validation scope.


## HIP xpoint search

C09 adds `keyhunt xpoint --backend hip` with exact half-open ranges, full 32-byte
X targets, CPU-verified matches and automatic candidate overflow replay. See
[the command, output contract and measurements](HIP_XPOINT.md). CPU-only builds
reject the HIP command explicitly; the legacy `-m xpoint` path remains separate.

```sh
ctest --preset hip-release -R xpoint
./build/hip-release/hip_xpoint_benchmark 0 65536 > /tmp/keyhunt-xpoint-benchmark.json
```

The opt-in benchmark compares direct multiplication with the default stepping
kernel after warm-up. It checks every result and reports preparation, seed,
kernel, download, CPU verification and submit-to-take wall timing. It measures
one logical device with volatile coverage, not durable or multi-device throughput.


## Versioned BSGS tables

C10 supplies a portable CPU builder/validator and an immutable HIP upload with
shared exact/filter lookup. The [format and operations guide](BSGS_TABLES.md)
defines memory budgets, checksum/semantic validation and the explicit legacy
cache rebuild policy. C11 implements [bounded HIP BSGS searches](HIP_BSGS.md).

```sh
./build/cpu-release/keyhunt bsgs-table build --m 4097 --output /tmp/babies.khb
./build/hip-release/keyhunt bsgs-table validate --backend hip --input /tmp/babies.khb
ctest --preset hip-release -R bsgs
./build/hip-release/hip_bsgs_benchmark 0 65537 > /tmp/keyhunt-bsgs-preparation.json
```

The benchmark checks positive/negative queries after warm-up and reports CPU
construction, upload, kernel, transfer and host validation times. It does not
measure giant-step or effective scalar-range search throughput.


## Bounded HIP BSGS search

The [C11 guide](HIP_BSGS.md) provides the command, target format, exact mapping,
replay/ownership rules and measured kernel selection. CPU-only builds retain
parser/mapping/verification tests and explicitly reject GPU execution.

```sh
ctest --preset hip-release -R 'bsgs_cli|hip_bsgs_search|bsgs_search_contract'
./build/hip-release/hip_bsgs_search_benchmark 0 65537 32768 > /tmp/keyhunt-bsgs-search.json
```

The search benchmark compares one/eight/automatic giant grouping with fixed m
and target sets. Actual target giant steps/s and effective scalar-range coverage/s
must be reported separately. Current search receipts are volatile.


## Local journal and assignments

C12 adds SQLite-backed storage to CPU and HIP builds and four CPU-only tests:
`storage_database`, `storage_journal`, `storage_concurrency` and `state_cli`.
The [storage guide](STORAGE.md) documents private external paths, manifests,
allocation policies, expiry/fencing, local JSON commands and quarantined restore.

~~~sh
ctest --preset cpu-release -L storage
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ctest --preset cpu-sanitizers -L storage
python3 tools/measure_c12_storage.py --binary build/cpu-release/keyhunt --report /tmp/keyhunt-c12-scaling.json
~~~

The opt-in measurement creates synthetic journals in temporary external
directories. It reports allocation wall time and sparse-index/file sizes for
sequential, global random and random-window claims. Timing includes process
startup, transaction, JSON and close/checkpoint; it is not GPU throughput.


## Verified local checkpoints

C13 adds `storage_checkpoint` and `storage_checkpoint_failures` to every CPU/HIP
suite. `checkpoint_cli` tests canonical job creation and explicit backend rejection
in CPU builds, and actual HIP crash/restart, overflow and result inspection in HIP
builds. These checks are included in the focused sanitizer selector.

~~~sh
ctest --preset cpu-release -R checkpoint
ctest --preset hip-release -R checkpoint
python3 tools/measure_c13_checkpoints.py --binary build/hip-release/keyhunt --oracle build/hip-release/secp256k1_oracle --report /tmp/keyhunt-c13-timing.json
~~~

See [CHECKPOINTS.md](CHECKPOINTS.md) for schema migration, command examples,
timing scope, known limits and acceptance evidence.

## Graceful checkpoint controls

C14 adds `storage_checkpoint_control` and `checkpoint_controls` to every preset.
They test the owner state machine and real Linux command/signal processes using
a bounded CPU fixture. The production executable has no test execution switch.
HIP builds also run `checkpoint_pause_hip` for real GPU pause, online snapshots,
graceful shutdown and exact restart with changed visible device counts.

```sh
ctest --preset cpu-release -R 'checkpoint|storage_database'
ctest --preset hip-release -R 'checkpoint|storage_database'
python3 tools/measure_c14_pause.py --binary build/hip-release/keyhunt --oracle build/hip-release/secp256k1_oracle --report /tmp/keyhunt-c14-pause.json
```

Run timing tools after hardware tests finish. The opt-in measurement excludes a
startup pause and retains five warm request-to-paused samples for each mode at
small and default launch sizes. [The operations guide](PAUSE_RESUME.md) records
the timing scope, hardware, recovery behavior and acceptance results.

## Authenticated coordination (C15)

`coordinator-release`, `coordinator-debug` and `coordinator-sanitizers` add the
optional coordinator and HTTPS worker without enabling HIP. `coordinator-server`
builds only the CPU service, disables worker/libcurl requirements and tests, and
installs with `--component coordinator`. All presets also require the existing
SQLite >=3.51.3 dependency; no build downloads dependencies.

```sh
cmake --preset coordinator-release -DKEYHUNT_TEST_APACHE_ROOT=/
cmake --build --preset coordinator-release -j12
ctest --preset coordinator-release
cmake --preset hip-release -DKEYHUNT_ENABLE_COORDINATOR=ON -DKEYHUNT_TEST_APACHE_ROOT=/
cmake --build --preset hip-release -j12
```

Supply `CMAKE_PREFIX_PATH` for custom JSON development packages and
`CURL_INCLUDE_DIR` / `CURL_LIBRARY` for a custom libcurl prefix. Apache is needed
only for live TLS integration and deployment, not to compile the service.
The normal CPU/HIP presets keep coordination disabled unless explicitly enabled.

[The coordinator guide](COORDINATOR.md) includes the private localhost launcher,
worker configuration, deployment templates, backup/restore rules and boundaries.
[Validation](COORDINATOR_VALIDATION.md) records the combined regression evidence.
The focused sanitizer selector continues to exclude only the pre-existing
`cpu_baseline` and `target_loading` legacy gates.

## C16 profiling and durability benchmarks

With the HIP test build (including `secp256k1_oracle`) already built, run:

```sh
python3 tools/benchmark_gpu.py --build-dir build/hip-release \
  --output-dir /tmp/keyhunt-c16-run --repeats 5 --batches 128
```

The output directory must be new and outside the checkout. The harness uses only
Python's standard library and local build tools; tracing additionally needs
`rocprofv3`. See [methodology, profiler commands and accepted evidence](GPU_PROFILING.md)
for exact workload units, durability interpretation, metadata, and retained raw
samples. Timing trials are opt-in; CPU CTest runs the benchmark acceptance checks.

## Native CUDA / H200 (C18)

Use the `cuda-h200` configure/build/test presets and explicit `--backend cuda`.
The [CUDA guide](CUDA_BACKEND.md) documents toolkit discovery, shared execution
contracts and H200 validation. HIP and CUDA are separate native builds.
