# Build and test

The search backend is the original Linux x86-64 CPU engine. Its assembly and
hashes require SSSE3. Optional HIP discovery and bounded diagnostics are now
implemented; GPU searches remain planned. `KEYHUNT_ENABLE_CUDA` still fails
configuration with an explicit C18 message. CPU builds never probe or download
either GPU SDK.

Use CMake 3.22+, GCC/G++ (11.4.0 tested), Make, and Python 3.9+ for tests. A
production-only build can omit Python with `-DBUILD_TESTING=OFF`. No dependencies
are fetched by the build. Clang is accepted but is not yet validated here.

```sh
cmake --preset cpu-release
cmake --build --preset cpu-release --parallel 4
ctest --preset cpu-release
./build/cpu-release/keyhunt -h
```

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
| `KEYHUNT_ENABLE_HIP` | OFF | AMD HIP discovery and diagnostic executor; requires ROCm AMD clang/runtime |
| `KEYHUNT_ENABLE_CUDA` | OFF | Explicitly rejected until C18 |

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
accessible GPU. `ctest --preset hip-release -L hardware` selects only the three
hardware checks; missing hardware fails those gates. The backend's device code
receives no CPU SIMD/native, fast-math or LTO flags. HIP+sanitizers is explicitly
rejected; use the separate CPU sanitizer build for host checks. Installation
includes the same `keyhunt` executable and uses the installed ROCm runtime.
See [HIP_BACKEND.md](HIP_BACKEND.md) for alternate SDK paths, partition identity
limitations, memory/ownership rules, and reproduction of the recorded evidence.
