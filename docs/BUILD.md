# Build and test

The implemented backend is the original Linux x86-64 CPU engine. Its assembly
and hashes require SSSE3. GPU support remains planned: requesting
`KEYHUNT_ENABLE_HIP` or `KEYHUNT_ENABLE_CUDA` currently fails configuration with
an explicit milestone message. CPU builds never probe or download either SDK.

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
