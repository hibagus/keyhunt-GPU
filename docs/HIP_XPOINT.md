# Bounded HIP xpoint search (C09)

The separate `xpoint` subcommand searches an exact half-open scalar interval using
full 32-byte X coordinates. It does not change the legacy `-m xpoint` semantics.
The GPU computes candidate points and performs exact target lookup; the preserved
CPU engine independently derives every returned match before it can be emitted.
Pinned libsecp256k1 supplies the end-to-end test expectations.

```sh
cmake --preset hip-release
cmake --build --preset hip-release --parallel 4
# X coordinate of G: both private scalars 1 and n-1 have this X coordinate.
printf '%s\n' 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 > /tmp/keyhunt-x.txt
./build/hip-release/keyhunt xpoint --backend hip --range 1:101 \
  --targets /tmp/keyhunt-x.txt --device 0 --batch-size 256 --candidate-capacity 32
```

`START:END` is hexadecimal, with an optional `0x` prefix. Require
`1 <= START < END <= n`; END is exclusive. No endpoint truncation, stride,
endomorphism, random order, early stop, or silent CPU fallback is enabled.
`--device` chooses one currently visible logical HIP device. CPX/QPX/SPX settings
are neither changed nor assumed; allocations query this device's current free
memory and retain 64 MiB of headroom. CLI defaults are 1,048,576 steps and 1,024
candidate slots; each limit must be 1..1,048,576.

## Targets and identity

A target file contains exactly 64 hexadecimal digits per nonempty line, with LF
or CRLF endings; the last newline is optional. Upper/lowercase digits are allowed.
The loader rejects embedded NULs, extra tokens, oversized lines, coordinates >= p,
empty target sets and more than 1,048,576 input records. An arbitrary canonical X
that is not on the curve is a valid no-match target. No decompression is required.

Targets are sorted by the full 256-bit coordinate and deduplicated before upload.
A SHA-256 digest of `xpoint-v1` followed by a NUL and the concatenated sorted
big-endian coordinates binds every work plan to its immutable target set. Target
indices in output refer to this sorted unique set; each match also carries X.
The X-only relation intentionally includes both Y signs, while distinct scalars
remain distinct matches. Lookup compares all 32 bytes, including low limbs.

## Ownership, overflow and progress

`HipXPointExecutor` owns one explicit nonblocking stream, fixed device/pinned
buffers, events and an immutable target upload. It retains the submitted C05
`KernelBatch`, including its work identity and exact bounds. Caller-owned CPU
verification context must outlive the executor. `submit`, `poll`, `drain` and
`take` follow C07's single-owner model; tickets share a namespace with diagnostic
executors. No unconsumed result is overwritten. Runtime or verification failures
poison the owner; destruction drains its stream before releasing buffers.

The device counts evaluated scalars and candidates and sets explicit overflow or
invalid-point flags. A guard follows the output capacity. The host checks the
count/flag relationships, guard, record bounds, uniqueness and CPU derivation.
Full coverage means every scalar was evaluated against the complete target set;
it does not mean CPU derivation was performed for nonmatches.

An overflowing attempt returns zero verified steps and no matches. The CLI keeps
its cursor fixed and retries with at most `min(capacity, attempted_steps/2)` steps,
retaining this smaller limit for later batches. Because targets are unique, each
scalar can produce at most one candidate; replay must therefore terminate without
silently dropping the retained prefix. Already accepted intervals are not replayed.

Output is newline-delimited JSON: a `start` record, bounded `batch` records, then a
`summary` only after the entire interval is accepted. Each accepted batch contains
CPU-verified matches sorted by scalar. The output stream is flushed and checked
before advancing the volatile cursor or submitting more work. Overflow records
have no matches/verified steps. Counts for whole jobs use exact 256-bit hex strings;
per-batch counts are bounded integers. Kernel, transfer, verification and wall
measurements are reported separately, including replay work in attempted counts.

These are **volatile execution receipts**, with `durable_coverage: false`, not
checkpoints or persisted assignments. A partial output after an error is not a
complete job. C12/C13 will add transactional durable matches and coverage; stdout
flush is not fsync. The application does not append legacy `KEYFOUNDKEYFOUND.txt`.
Host storage is bounded by targets and one result slot, independent of range size.

## Reference implementation and validation

The initial reference kernel maps each thread to `begin + index`, with all eight
scalar limbs and a widened grid product. It performs checked GPU scalar
multiplication, normalizes only X and uses binary exact lookup. Every valid lane
increments the evaluated count after lookup. Threads beyond a partial block do
nothing. This deliberately simple path supplies a measured comparison for the
stepping optimization; it is not advertised as maximum throughput.

Tests cover independent oracle matches at starts/middles/ends, no-match inputs,
full-X negatives, carries across 32/64/128/192/255 bits, the order endpoint,
exhaustive dense partial batches, duplicates, forced capacity-one replay, every
visible device, output ownership and foreign/stale tickets. Test-only runtime
failure and downloaded-data corruption hooks verify rejection before acceptance;
they are absent from the production library. The host target/candidate contract
also runs in CPU release/debug and ASan/UBSan builds.

[Reference acceptance evidence](baselines/C09_REFERENCE.json) records source hashes,
41 independent CLI searches, 28 rejection cases, the focused HIP/host suite and
host sanitizer results. Twenty-nine xpoint runtime/corruption boundaries pass.
The fault-test binary uses the host C++ linker because the CPU archives contain
GCC LTO objects; HIP clang/lld cannot consume those archives directly. Kernel
compilation remains HIP without the legacy x86/fast-math compiler options.


## Measured point-stepping optimization

`--kernel stepped` is the default; `--kernel direct` retains the validated
scalar-per-thread reference. A stepped lane owns eight consecutive offsets,
starting at `8 * global_thread_index`. The CPU computes the batch's `begin*G`
once. A read-only table of the 20 affine points `2^i * G` is prepared once per
executor (1,920 bytes); its set bits reconstruct the lane offset without another
256-bit multiplication. The lane then adds G for the remaining seven points.
The final group clips to the exact batch count; no point outside the range is
looked up. Equal/inverse/infinity cases retain C08's complete point operations.
No wave size, CU count, XCC count or partition count enters this mapping.

For one to four targets, compare Jacobian `X` with `target_x * Z^2` directly;
nonzero Z makes this equivalent to affine-X equality. This removes normalization
inversions. Larger target sets keep eight X/Z pairs and use C08 batch inversion,
then the same exact binary lookup. Separate template instantiations remove the
unused arrays from the small-target path. Count atomics are amortized per lane
group; candidate writes and overflow semantics are identical to the reference.

The paired benchmark warms each executor once, alternates direct/stepped order
and retains five samples for each equivalent workload. All samples check exact
counts and expected matches, including CPU verification. Tables/uploads are
prepared outside the timed batches and their preparation costs are reported.
The first owner can pay lazy runtime initialization, so those preparation samples
are descriptive and are not used for the speedup comparison.
Batch wall time includes CPU seed derivation, queueing, download and verification;
CLI startup, JSON output and durable storage are excluded. Kernel events exclude
CPU seed work. The host was not reserved and clocks/power limits were not changed.

| Scalars | Targets/workload | Direct median wall ms | Stepped median wall ms | Speedup |
| ---: | --- | ---: | ---: | ---: |
| 65,536 | 1, no match | 3.273747 | 0.408086 | 8.02x |
| 65,536 | 3, start/middle/end matches | 3.284686 | 0.447317 | 7.34x |
| 65,536 | 32, no match | 3.276173 | 0.781553 | 4.19x |
| 1,048,576 | 1, no match | 41.316840 | 0.708322 | 58.33x |
| 1,048,576 | 3, start/middle/end matches | 41.584321 | 0.754030 | 55.15x |
| 1,048,576 | 32, no match | 41.656146 | 1.912843 | 21.78x |

Use the maximum bounded batch (1,048,576) by default: its 16 times larger work
amortizes launch overhead and exposes more independent groups on this SPX device.
Smaller ranges still launch only their actual work; `--batch-size` can reduce the
limit on other partitions/workloads. This choice does not alter memory capacity
or exact intervals, and all executor allocations remain explicitly bounded.
Retain the stepped path based on these whole-batch improvements, not a claim of a
global optimum. C16/C17 must revisit grouping, block size, arithmetic, spills,
large target layouts and scheduling with profiler evidence and durable workloads.

Reproduce the checked measurements (one visible logical device at a time):

```sh
./build/hip-release/hip_xpoint_benchmark 0 65536 > /tmp/keyhunt-xpoint-small.json
./build/hip-release/hip_xpoint_benchmark 0 1048576 > /tmp/keyhunt-xpoint-large.json
```


Compiler resource remarks for these exact gfx942 kernels:

| Kernel | SGPRs | VGPRs | Scratch bytes/lane | VGPR spills | LDS bytes/block |
| --- | ---: | ---: | ---: | ---: | ---: |
| Direct | 86 | 128 | 164 | 84 | 32768 |
| Stepped, up to 4 targets | 104 | 128 | 260 | 220 | 0 |
| Stepped, larger target set | 108 | 128 | 912 | 105 | 32768 |

All three report an estimated four waves/SIMD. These are compiler estimates,
not achieved occupancy or profiling counters. The stepped path wins despite
spills because it removes much more arithmetic; the remaining scratch traffic
and compiler-generated LDS use are concrete C17 tuning targets. No handwritten
ISA, wave-width assumptions, fast-math or architecture dispatch was added.


[Measurement evidence](baselines/C09_MEASUREMENTS.json) retains raw samples,
min/median/max statistics, source/binary fingerprints, exact compile commands,
runtime/device identity, compiler remarks, profiler version and read-only clock,
power and temperature snapshots around the final runs. Clocks were not pinned;
snapshots do not prove a constant frequency during every sample.

[Final search parity evidence](baselines/C09_SEARCH.json) records 48 independent
search cases per kernel and 29 CLI rejections per variant, including full-capacity
local-offset bits and both target-lookup paths. Both variants execute on all eight
visible SPX/NPS1 devices. Thirty-one runtime/corruption boundaries pass, including
the extra seed-table allocation/upload. CPX/QPX compatibility remains mode-independent
by construction and covered by C07 discovery tests; actual C09 CPX/QPX hardware
search validation is pending. Partition settings remain unchanged.
