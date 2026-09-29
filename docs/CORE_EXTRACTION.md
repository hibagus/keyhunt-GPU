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
