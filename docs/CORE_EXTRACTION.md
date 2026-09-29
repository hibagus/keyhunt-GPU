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
