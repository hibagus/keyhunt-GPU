# C02: source organization and CMake migration

This change moves production files and adjusts includes/build paths. The
monolithic CLI, worker loops, arithmetic and result formats are unchanged.
Extraction into core/backend interfaces remains C04 and later work.

## Layout and provenance

| Original location | New location |
| --- | --- |
| `keyhunt.cpp` | `src/app/keyhunt.cpp` |
| `util.c`, `util.h` | `src/core/util.cpp`, `include/keyhunt/core/util.h` |
| `secp256k1/*.cpp`, `hash/*.cpp` | `src/crypto/secp256k1/`, `src/crypto/hash/` |
| Shared arithmetic/hash headers | `include/keyhunt/crypto/` |
| `base58`, `bloom`, `oldbloom`, `rmd160`, `sha3`, `xxhash` | `third_party/`, preserving embedded and standalone notices |
| `keyhunt_legacy.cpp`, `gmp256k1`, `hashing.c`, `hashing.h` | `legacy/` (`hashing.cpp` reflects its existing C++ compilation) |
| `bsgsd.cpp` | `legacy/bsgsd.cpp` |
| Original test inputs | Existing `tests/` paths retained |
| Future device code | `kernels/README.md` records the intended split; no kernels are implemented yet |

The exact [54-file move map](baselines/C02_SOURCE_MOVES.json) permits review
against C01 commit `fcfe55f`. A byte comparison excluding only `#include` lines
confirmed no other source/header content changed. Original mixed line endings,
comments, embedded assembly and license notices are preserved. CMake still
compiles SHA-3 `.c` files as C++, as the original Makefile did.

The ownership audit found GPLv3 notices in the maintained CPU arithmetic/hash
sources and mixed notices in GMP legacy code. The root MIT license is retained;
it does not supersede those per-file notices. See
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for evidence and incomplete
upstream notice records. No code from the CUDA/Apple reference was imported.

## Build decisions

CMake 3.22 and C++17 match the plan. Targets separate shared utilities/vendor
code, main CPU crypto, optional legacy crypto, and executables. GMP and OpenSSL
are discovered only for legacy. Threads are linked through `Threads::Threads`.
CPU configuration needs no HIP/CUDA SDK and makes no network request.

The three CPU presets cover release, debug, and sanitizers. HIP/CUDA options
fail clearly until those implementations exist; placeholder GPU executables
would misrepresent current capabilities. Build/install instructions are in
[BUILD.md](BUILD.md). The root Makefile is a small compatibility wrapper.

An initial whole-target IPO build stalled before printing the startup banner.
Disabling whole-target IPO restored the bounded xpoint test. The final build
preserves **only the original Makefile's selected LTO sources**, and passes the
full baseline. This is an observed compiler/build interaction, not a diagnosed
root cause. Audit it before changing LTO scope during optimization. The release
build also retains native tuning, SSSE3, `-Ofast` and vectorization. CMake Debug
was tested with `KEYHUNT_NATIVE_CPU=OFF` as a distinct compiler configuration.

## Validation and observed defects

- Release: all 38 C01 characterization cases pass through CTest.
- Debug, native tuning disabled: all 38 cases pass through CTest.
- Optional main, legacy and bsgsd targets build together without symbol clashes.
- Original and reorganized legacy binaries agree on 13 selected comparisons;
  positive cases require result files, preventing two failed launches from
  being counted as parity. One specifically identified old parser defect is
  compared as a rejection: the valid uncompressed `0x100001` public key is
  rejected by both legacy builds as off-curve. The main engine accepts it.
- The original and reorganized daemon both answer a known match, a no-match
  request and a malformed request correctly on loopback. Its `-p` option
  currently changes the printed port but `bind()` uses constant `PORT` (8080).
  This real pre-existing defect is documented, not folded into the moves.
- Explicit HIP/CUDA requests fail with the intended “not implemented” message.
  Requesting legacy without GMP headers reports the required development
  package or explicit `GMP_INCLUDE_DIR`/`GMP_LIBRARY` settings.
- The sanitizer preset compiles. Its bounded xpoint probe fails with existing
  misaligned integer accesses and LeakSanitizer's 20,560 bytes in three
  allocations (`thread_process`, `IntGroup`, range tokenization). The report
  remains a failure; no sanitizer suppression or production workaround was
  introduced. Resolve these defects in separate changes as core ownership and
  arithmetic validation are introduced.

The original legacy Makefile failed in C01 because this host lacks `gmp.h`.
For C02, Ubuntu's `libgmp-dev` package `2:6.2.1+dfsg-3ubuntu1` was downloaded and
extracted under `/tmp/keyhunt-c02-deps/sysroot`, without installing it globally.
The original build used its include/library directories through process-local
`CPATH` and `LIBRARY_PATH`. CMake used explicit `GMP_INCLUDE_DIR` and
`GMP_LIBRARY` pointing at the extracted headers and static library. The system
OpenSSL development files are version 3.0.2. This resolves the legacy validation
gate for the migration; users still need their own development dependencies.

Raw validation records are grouped in [C02_VALIDATION.json](baselines/C02_VALIDATION.json).
The failing sanitizer [probe](baselines/C02_SANITIZER_PROBE.json) is retained for
follow-up. Historical C01 records keep their original paths and results.
These tests establish compatibility across the moves, not GPU or exhaustive
coverage correctness. C01's range defects remain explicitly characterized.
