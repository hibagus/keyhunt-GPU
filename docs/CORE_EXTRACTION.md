# C04: host core extraction

C04 separates reusable host responsibilities from the preserved CPU application.
It is split into three commits: configuration, target loading, and result
verification. The CPU CLI remains the compatibility adapter; this milestone does
not replace its search loops or certify their range coverage.

## Configuration

`include/keyhunt/core/config.h` defines `SearchConfig` and the established mode,
crypto and encoding values. `src/core/config.cpp` owns their CLI names. The
application now reads these preferences through one configuration object rather
than separate globals. Filename, stride text and batch-size text own their storage.
The new `keyhunt_core` CMake target links to the existing CPU crypto implementation.
Its public configuration header needs only the C++ standard library.

Numeric option values, argument-order effects, diagnostics, defaults and fallback
behavior are preserved. Integer flags deliberately retain the old CPU dispatch
representation. `getopt`, presentation, minikey buffers, normalized arithmetic
ranges and statistics timers remain in the application. The C05 exact-range
contract will replace the old range normalization for scheduled execution; moving
that code unchanged would reproduce the known C01 coverage defects.

Validation: release build succeeds and all 38 C01 CLI characterization cases pass.
The baseline covers modes, key encodings, ranges, stride, random/minikey execution,
invalid inputs and the existing boundary defects. Source line numbers in error
messages are not stable across extraction.

## Findings for subsequent C04 commits

The current target table retains only 20 bytes of an X coordinate. BSGS final
checks sometimes compare X alone, and its loader indexes a vector after `reserve`
without constructing elements. Target ownership and the distinction between a
prefilter and exact verification must be explicit. Native cache files are still
CPU compatibility artifacts, not portable or authenticated target-set formats.

The new verifier must recompute candidates on the CPU, reject invalid scalars,
and support full 32-byte X and both public-key coordinates. The existing CPU
arithmetic remains a regression dependency; C06 supplies the independent oracle.

## Target loading

`CpuTargetTable` now owns the CPU text/native-cache table and its Bloom allocation.
Its configuration is an explicit reference, and its count/cache status are
separate from BSGS state. Bitcoin/hash160, Ethereum, xpoint, BSGS and vanity file
reading live in `src/core/cpu_targets.cpp`. Vanity prefix compilation remains in
the application, passed to the file reader as a callback. BSGS target points and
encoding flags have their own owned container. The loaders no longer depend on
application globals or GPU headers.

The BSGS target vector is sized before indexed parsing; its old `reserve` call
allocated storage without constructing elements. Temporary BSGS line storage and
Bitcoin input descriptors are now released. Native-cache pointer bytes are
cleared immediately after reading the header, so a partial read cannot leave the
owning object holding a pointer from disk. The format and checksums are unchanged;
this is not a hardened importer for untrusted cache files.

Release validation: all 38 baseline cases and five new integration cases pass.
The latter check xpoint/Bitcoin/hash160/Ethereum cache creation and reuse, reject a
corrupted checksum without emitting results, and load mixed valid/invalid BSGS
points with both encodings. See [loader evidence](baselines/C04_TARGET_LOADING.json).

An additional debug build with `-D_GLIBCXX_ASSERTIONS` found an existing startup
failure before target loading: `init_generator` indexes `Gn` after `reserve`, like
the old BSGS loader. Inspection found the same pattern in `GSn`, `BSGS_AMP2` and
`BSGS_AMP3`. [The first assertion](baselines/C04_DEBUG_ASSERTION.json) is saved for a
separate correctness fix; this diagnostic run is not a passing debug gate.

## Follow-up: construct generator table elements

A separate fix changes `reserve` to `resize` for `Gn`, `GSn`, `BSGS_AMP2` and
`BSGS_AMP3`. All four are filled by index and require live `Point` objects.
Debug builds now define `_GLIBCXX_ASSERTIONS` for C++ targets, which detects this
class of error when building with libstdc++.

After the fix, both release and debug pass all 38 baseline cases and the five
loader integration cases. Debug runs with the new bounds checks enabled (40.43s
for both suites); release takes 37.85s. The earlier failure record is historical:
the process aborted with SIGABRT without emitting assertion text on this host.
The failing reserve/index pattern was identified by source inspection and the
passing run after resizing. This does not resolve the separate C02 alignment and
leak sanitizer findings.

## CPU result verification

`result_verifier.h` provides fixed-size, big-endian scalar, X, public-key and
hash byte arrays. `CpuResultVerifier` recomputes a candidate using the initialized
CPU curve and checks a full 32-byte X, compressed or uncompressed public key,
Bitcoin hash160 with an explicit encoding, or Ethereum address bytes. It rejects
zero and scalars at or above the curve order without reducing them. Failed
`derive` calls leave the caller's output unchanged. The public header uses only
standard-library types and a forward declaration of the CPU curve.

`cpu_result_adapter.h` connects the existing engine to this verification code.
BSGS scalar reconstruction and all five BSGS output paths now check both X and Y;
the infinity shortcut is also rechecked. Bitcoin/hash160, Ethereum and minikey
output paths recompute candidates against the loaded CPU table. Ethereum address
derivation has moved out of the application. Reverification runs only for candidate
matches, apart from the existing Ethereum derivation in the search loop.

The compatibility xpoint table still verifies its historical 20-byte prefix.
**Full-width GPU verification must use `matches_xpoint` and retain all 32 target
bytes.** The native CPU cache cannot supply those missing bytes. Vanity prefix
matching/output remains in the application. Target parsing is still the historical
CPU parser, not a canonical portable target-set format. These are explicit
adapter limits, not guarantees of exhaustive or fully strict CPU searching.

The verifier accepts arbitrarily aligned public byte buffers. Aligned temporaries
bridge the old `Int` byte importer/exporter, and hash160 uses existing general
SHA-256/RIPEMD-160 functions over serialized bytes rather than the search loop's
specialized unaligned SEC1 writer. Arithmetic and hash implementations themselves
are unchanged. Initialize the shared curve before any worker starts; verification
uses per-call temporaries and must not race another curve initialization.

A verified match establishes only a scalar-to-target relationship. C05 supplies
exact range membership, and later execution/persistence stages must check work
identity, deduplicate results, make them durable and decide coverage. This API
cannot acknowledge completed work. C06 still needs an independent pinned oracle.

## Final validation and reproduction

[Validation summary](baselines/C04_VALIDATION.json) records binaries, commands and
outcomes. Release and debug pass 38 CLI baseline cases, five loader cases and 55
verifier assertions. Verifier cases include zero/order/over-order scalars, a scalar
above 64 bits, order-minus-one, mismatched Y/encoding/X suffixes, unchanged output
on failure, unaligned API buffers and CPU-adapter rejection of negative/over-wide
integers. Optional legacy xpoint and daemon loopback checks pass, as do both README
examples. The host configuration/verifier headers compile without GPU SDK headers.

```sh
cmake --build --preset cpu-release --parallel 4
ctest --preset cpu-release
cmake --build --preset cpu-debug --parallel 4
ctest --preset cpu-debug
```

The focused sanitizer run **does not pass** with `UBSAN_OPTIONS=halt_on_error=1`:
`IntMod.cpp:694` overflows signed arithmetic in `Int::SetupField` during curve
initialization. An unsuppressed recovery run reaches all 55 verifier assertions,
but also reports inherited signed arithmetic overflow in `SetupField` and
`ModInv`; reaching those assertions is not a clean sanitizer result. The
[complete diagnostic log](baselines/C04_VERIFIER_SANITIZER.log) is retained for
C06. No sanitizer suppressions or arithmetic rewrites were added. The broader
CPU application's previously recorded alignment and leak issues also remain.

```sh
cmake --preset cpu-sanitizers
cmake --build --preset cpu-sanitizers --parallel 4 --target result_verifier_test
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset cpu-sanitizers -R '^result_verifier$'
```

C06 follow-up: [independent field checks](ARITHMETIC_ORACLE.md#cpu-field-arithmetic-corrections)
reproduced and corrected modular reduction and inversion issues. The focused
verifier sanitizer gate above now passes; this section's C04 failure log remains
historical evidence. Application alignment/leak findings are still separate.
