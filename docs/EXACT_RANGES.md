# Exact ranges and work planning (C05)

The new host scheduling contract uses nonempty half-open scalar intervals
`[begin, end)`, with `1 <= begin < end <= n` for the secp256k1 order `n`. It is
separate from the preserved CPU CLI and search loops. In particular, old `-r`,
stride, random search and endomorphism behavior do not establish exhaustive
coverage for this contract. See [the CPU baseline](CPU_BASELINE.md) and
[the scheduling design](GPU_REDESIGN_PLAN.md#separate-ownership-local-work-and-gpu-batches).

## Checked integers and scalar intervals

[UInt256 and ScalarInterval](../include/keyhunt/core/exact_range.h) are host-only
types in the `keyhunt_ranges` library. They do not include GPU, GMP, or existing
curve arithmetic headers. The library is linked through `keyhunt_core` for future
executors and can be tested without initializing the legacy arithmetic engine.

Values serialize as exactly 32 bytes in big-endian order. Canonical text is
`0x` followed by 64 lowercase hex digits. Parsing accepts 1–64 hex digits with
an optional `0x`/`0X` prefix, including uppercase digits, then normalizes on output.
Empty text, signs, whitespace, invalid digits and over-wide values are errors;
there is no silent truncation or modular reduction. These are hex values, even
when their spelling contains only decimal digits. Future protocol parsers must
enforce their canonical wire representation before hashing manifests.

Addition checks the carry beyond 256 bits, subtraction checks borrow, and
multiplication checks a complete 512-bit intermediate. Division retains an
explicit high remainder bit. All limb operations use unsigned integers; no
floating-point conversion is involved. Conversion to a local 64-bit count rejects
overflow. Errors throw standard exceptions without changing operands.

Scalar intervals reject zero starts, reversed or equal endpoints, and endpoints
above `n`. The order is a legal exclusive endpoint and is never a legal candidate.
An empty allocation will be represented by absence, not an invalid interval.
Membership tests accept the verifier's 32-byte scalar representation through
`UInt256::from_bytes()` and check the exact half-open bounds.

`UInt256::power_of_two(b)` supports bits 0–255. Widths up to `2^256-1` fit the
public format and already exceed the entire valid scalar domain. A `2^256` width
is rejected instead of wrapping; future `--block-bits` must enforce this bound.

## Validation

`exact_range` tests parsing, byte order, all 256 single-bit/carry boundaries,
overflow rejection and scalar-domain membership. `range_oracle` compares C++
results to Python's independent arbitrary-precision integers with seed
`0xC05E7AC7`: exhaustive small arithmetic and interval domains, wide boundaries,
and randomized wide operands. The probe is test-only and not a product CLI.

```sh
cmake --preset cpu-release
cmake --build --preset cpu-release --parallel 4
ctest --preset cpu-release -L ranges
```

The first change passed 1,059 unit checks and 22,607 oracle comparisons in
release, debug and ASan/UBSan builds on the C01 host. Sanitizers used
`ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1` without suppressions.
[Recorded integer evidence](baselines/C05_INTEGER_VALIDATION.json) includes the
seed, categories and binary hashes. This clean result applies to the isolated
range library; the inherited curve sanitizer findings remain open in
[CORE_EXTRACTION.md](CORE_EXTRACTION.md).

Block-grid and bounded work-unit planning are the remaining C05 changes. No
checkpoint, ownership, completion accounting or search execution is introduced
by this integer contract.
