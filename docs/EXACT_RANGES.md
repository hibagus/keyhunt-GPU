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

## Immutable block geometry

[BlockGrid](../include/keyhunt/scheduler/block_grid.h) owns a root interval and a
positive, immutable width. Counts and IDs remain `UInt256`. It computes the block
count using quotient and remainder, avoiding overflow in `span + width - 1`.
A lookup rejects `id >= count` before multiplying, computes the full checked
product, and clips the remaining span before adding the end. A width larger than
the root yields one shorter block. Looking up an ID takes constant storage even
when the job contains more than `2^64` blocks. There is no block table allocation.

`block_containing(scalar)` gives the inverse mapping and rejects a scalar outside
the job, including its exclusive endpoint. For the decimal example `[1000,1100)`
with width 32, the four blocks end at 1032, 1064, 1096 and 1100. A block lookup
says nothing about whether it is unexplored, in progress or finished; the sparse
journal and assignment transactions belong to C12.

## Bounded work units and kernel batches

[WorkUnit and KernelBatch](../include/keyhunt/scheduler/work_unit.h) subdivide a
selected block lazily. A work unit owns an exact interval, parent block bounds
and 256-bit block ID, job/target/configuration digests, assignment ID, assignment
generation and local executor generation. Kernel batches retain that complete
parent description. Identity comparisons include every field. Digests are opaque
caller-supplied values in C05; canonical manifest construction is later work.

The initial mapping is explicitly `DirectXPointV1`: one consecutive scalar per
step, `scalar = batch.begin + local_index`. Unknown mappings are rejected. This
is planning support for C09, not an implemented search backend. BSGS's scalar
span versus giant-step count, tile alignment, target groups and residual tails
need C11's separate mapping. No existing stride, endomorphism, random search or
minikey execution is silently admitted as exhaustive contiguous coverage.

Both planning functions take an explicit cursor and positive `uint64_t` step
limit. They reject cursors outside the parent, return no allocation at its
exclusive end, and clip the requested span before adding an endpoint. Work-unit
and kernel-batch step counts each fit 64 bits; absolute scalars, block IDs and
logical widths retain all 256 bits. `scalar_at(index)` checks `index < count`
before adding it to the full scalar. GPU grid products and bounds must receive
separate device-side checks in the executor; this host method does not validate
a launch or device arithmetic.

There is deliberately no automatic cursor advancement or completion method.
Repeated planning with the same inputs yields the same bounds for safe replay.
The future owner must supply a cursor derived from committed coverage and persist
exact in-flight bounds; passing an arbitrary later cursor does not prove that the
prefix was searched. Resume can plan the remaining suffix of a recorded work
unit from an interior batch cursor. New units may have a different size without
changing the existing unit or the job's block grid.

The 12-hour logical block target, 180-second local work target, 0.1–1-second kernel
target, approximately 10-second local checkpoint cadence, 2-hour machine sync and
30-day reservation remain the plan's independent controls. C05 accepts explicit
widths/counts; it does not infer a measured GPU rate or implement timers. No
network exchange is necessary to calculate work inside a block. One GPU per
active block, assignment ownership, expiry, and sparse unexplored/in-progress/
finished state still require their scheduler/journal/coordinator milestones.

Nonzero assignment IDs and positive generations are syntactically required.
Their authority, current generation, lease and matching manifest must be checked
by the future owner; constructing a work unit is not authorization. A completion
will also need its exact parent unit/batch interval and execution identity checked,
CPU-verified in-range candidates saved, overflow replayed, and accepted coverage
committed before advancing. These result and persistence gates belong to C09 and
C12–C15. Work-unit objects are not checkpoint serialization or proof of coverage.

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

The block-grid change passed 197,479 unit checks, including actual enumeration of
each scalar in small jobs exactly once. The expanded oracle passed 53,261 total
comparisons, including random 256-bit block lookups and shorter tails, in release,
debug and the same unsuppressed sanitizer configuration. See
[recorded grid evidence](baselines/C05_GRID_VALIDATION.json).

`work_unit` enumerates every candidate through all three levels for small jobs,
asserting exactly one visit per scalar. It also tests identity snapshots, explicit
replay/resume cursors, changes to new work sizes, all invalid cursor/index limits,
256-bit starts and IDs, `UINT64_MAX` counts, and tails ending at the curve order.
The expanded Python oracle independently checks work and batch bounds and scalar
reconstruction across exhaustive small and seeded wide domains.

Final C05 validation passed all seven CTest suites in release (39.88 seconds) and
debug (49.40 seconds). This includes 38 original CPU cases, five loader cases,
55 verifier checks, 1,059 integer/interval checks, 197,479 block checks, 191,517
work-planning checks and 111,829 Python-oracle comparisons. All four new planning
suites also passed ASan/UBSan with leak detection and halt-on-error enabled
(20.26 seconds), without suppressions. These are test-suite timings, not search
throughput measurements. [Final evidence](baselines/C05_VALIDATION.json) records
commands, full CTest summaries, oracle categories and binary hashes.

The new oracle harness initially failed to write its report because a work-argument
tuple shadowed the CLI options variable; separating the variable names fixed the
harness. All final comparisons and suites above passed after that correction.
The existing CPU arithmetic sanitizer defects remain separate C06 follow-up work.
Hosted CI has not been run here; the existing CPU workflow will pick up these
CTest additions on a future push.

C05 was split into checked integers/intervals (`ca82e05`), immutable block geometry
(`1910d90`), and bounded work/batch planning, each with its tests and documentation.

This is a host planning library, without new user-facing CLI commands. The CPU
compatibility path retains its C01 behavior. New scheduled execution must use this
contract; old CPU output cannot be imported as coverage just because a candidate
passes the C04 cryptographic verifier.

## Positive scalar strides

C23 adds a separate immutable affine mapping for xpoint, HASH160/P2PKH, Ethereum
and vanity. `--range A:B --stride S` visits `A+i*S<B` with checked 256-bit integer
arithmetic. For `S>1`, one-based candidate indices `[1,N+1)` define every block,
work unit, batch and saved interval, where `N=1+floor((B-A-1)/S)`. The private scalar
at index `j` is `A+(j-1)*S`. Block width counts candidates. The original range and
stride bind job and execution identity; CPU verification maps indices before
checking targets. Stride one retains consecutive-scalar identities and records.
See [contracts and compatibility](C23_STRIDES.md). This does not adopt legacy `-I`
loop behavior, endomorphism or random traversal as exact coverage.

## Reverse scalar order

For the scalar families, `--order reverse` reverses the exact forward lattice:
`N=1+floor((B-A-1)/S)` and `scalar(j)=A+(N-j)*S`, for `j` in `[1,N+1)`.
Bounds and positive stride remain unchanged. A short tail does not shift the
lattice to `B-1`. Candidate indices advance even as actual scalars decrease;
this also applies to stride one. Blocks, complements and receipts use indices.
Direct subtraction cannot wrap; only stepped point-cache arithmetic is modular.
See [reverse contracts](C23_REVERSE.md) and [validation](C23_REVERSE_VALIDATION.md).
