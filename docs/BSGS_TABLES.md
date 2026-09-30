# Versioned BSGS table preparation (C10)

C10 prepares immutable baby-step tables for C11's HIP BSGS search. It does not run
a BSGS range search or report search coverage. CPU generation and semantic cache
validation supply the reference; the HIP preparation/lookup layer uses the same
portable format, hash and filter contract. Existing legacy `-m bsgs` and `-S`
caches retain their original behavior and are never reinterpreted as this format.

```sh
./build/cpu-release/keyhunt bsgs-table build --m 4097 --output /tmp/babies.khb
./build/cpu-release/keyhunt bsgs-table inspect --input /tmp/babies.khb
```

Both commands work without a GPU SDK. `--m`, `--bits-per-entry` and `--host-memory`
are unsigned decimal values. The default host budget is 1 GiB and the filter
policy is 16 bits per entry; 8 and 32 are also supported. The supplied path must
not already exist for `build`. `inspect` validates the entire file, including
all baby-point relations; it is not a header-only inspection. JSON reports the
geometry, budget, checksum, elapsed time and `search_coverage: false`.

## Baby steps and exact lookup

The table contains exactly `m` records, `jG` for `0 <= j < m`. `m` and `j` are
unsigned 64-bit integers, with checked size calculations before any allocation.
All these finite scalars are below the secp256k1 order. Step zero is infinity,
encoded as 33 zero bytes; finite keys are SEC1 compressed points: 02/03 followed
by the full 32-byte big-endian X coordinate. Both Y signs remain distinguishable.

For C11, the residual point is `Q - (a + i*m)G`; an exact hit gives
`k = a + i*m + j`. C11 must reconstruct and range-check this value with exact
scalar arithmetic and CPU-verify the complete public point. No X-only symmetry,
truncated-key lookup, signed-index shortcut, or automatic change of m is enabled.

Records are sorted by `(hash(key) & (bucket_count-1), key, j)`, with a contiguous
span for every bucket. Bucket count is the next power of two at least `ceil(m/8)`.
There are no linked pointers, occupancy limits or capped collision probes.
Lookup binary-searches both ends of the equal-key list and returns every matching
record. Even though actual v1 baby points are unique, the primitive supports equal
keys with distinct indices; synthetic tests exercise these lists and indices
above 32 bits. Opposite signs and different low X bytes are exact negatives.

## Hash and Bloom version 1

Hash all 33 bytes with 64-bit FNV-1a (offset `cbf29ce484222325`, multiplier
`100000001b3`), then apply:

```text
x = (x xor (x >> 30)) * bf58476d1ce4e5b9
x = (x xor (x >> 27)) * 94d049bb133111eb
x =  x xor (x >> 31)
```

All arithmetic wraps modulo 2^64. This is a noncryptographic lookup hash, not a
checksum. Let h be the result, and d = `mix(h xor 9e3779b97f4a7c15) | 1`.
The seven filter positions are `(h + i*d) & (bit_count-1)`, i=0..6, with unsigned
64-bit wrap. `bit_count` is the next power of two at least
`max(64, m * bits_per_entry)`. Bit b is `1 << (b % 64)` in word `b / 64`.
Construction and query share `include/keyhunt/core/bsgs_layout.h`; independent
Python tests reproduce the constants, overflow and bit ordering from scratch.

A Bloom hit is only permission to perform exact lookup. Loading rejects every
false negative. Extra set bits, including an all-one filter, remain safe and are
accepted; they affect performance and the checksum but cannot introduce a match.
CPU/GPU comparisons must check exact matches as well as filter decisions.

## File layout (version 1)

All integers are explicitly little endian. Point coordinates remain big endian.
No C++ padding, pointers, native `size_t`, or legacy Bloom structs are serialized.

| Header offset | Width | Value |
| ---: | ---: | --- |
| 0 | 8 | `KHBSGS1` followed by NUL |
| 8 | 4 | Format version: 1 |
| 12 | 4 | Header size: 128 |
| 16 | 4 | Curve ID: 1 (secp256k1) |
| 20 | 4 | Mapping ID: 1 (jG, 0 <= j < m) |
| 24 | 4 | Entry format: 1 (full compressed point plus uint64 j) |
| 28 | 4 | Integer byte order: 1 (little endian) |
| 32 | 4 | Hash ID: 1 (specified FNV-1a plus mix) |
| 36 | 4 | Bloom probes: 7 |
| 40 | 8 | m |
| 48 | 8 | Bucket count |
| 56 | 8 | Bloom word count |
| 64 | 4 | Requested bits per entry: 8, 16 or 32 |
| 68 | 4 | Entry size: 48 |
| 72 / 80 / 88 | 8 each | Offset / entry / filter section byte sizes |
| 96 | 8 | Total payload bytes |
| 104 | 24 | Reserved zero bytes |

The payload is `(bucket_count+1)` uint64 offsets, then m 48-byte entries, then
`bloom_words` uint64 words. An entry is the 33-byte point key, seven reserved zero
bytes, and uint64 j at entry offset 40. A 32-byte SHA-256 trailer covers the
complete header and payload. Trailing bytes are an error. The checksum is the
cache identity; it is neither an authenticity signature nor a substitute for
semantic validation.

Loading validates versions, sizes, reserved bytes, file length, checksum, ordered
bucket spans, unique complete baby indices, every CPU-derived jG key and all
filter memberships. Even a malformed file with a recomputed valid checksum is
rejected. A changed bit policy is an explicit different compatible table, never
an unnoticed response to memory pressure. Legacy or unknown-format files produce
an explicit rebuild error; there is no importer in C10.

## Memory and publication decisions

Resident bytes are `48*m + 8*(buckets+1) + 8*bloom_words`. The conservative host
peak additionally includes a full encoded file buffer, `ceil(m/8)` validation
bitmap, the CPU curve context and 64 KiB of stack/I/O allowance. Geometry and
size_t/stream-size bounds are checked before allocation. This is a budget for
these table operations, not a hard process-RSS limit; runtime/allocator overhead,
caller-owned data and other workers need their own headroom. It never silently
reduces m or borrows another logical device's budget.

CPU derivation uses the previously validated generator table once per build or
load. Host bucket sorting and filter construction are preparation costs, not
GPU search throughput. Cache reuse amortizes this work; GPU generation/layout
specializations require a separate measured parity gate. C11 can share a prepared
immutable upload across compatible target groups without rebuilding it per batch.

Publication writes an exclusive temporary file beside the destination, fsyncs it,
then links it to the absent destination, removes the temporary name and fsyncs
the parent directory. Existing files (including symlinks) are never overwritten.
An error after publication can leave a complete file; inspect it before retrying.
This cache publication does not implement search checkpoints or durable coverage.


[Host format evidence](baselines/C10_FORMAT.json) records the independent Python
format/hash/filter reconstruction and pinned libsecp256k1 baby-point checks,
corrupted/rehashed cache rejections, exact-budget boundaries, collision lists and
focused release/ASan/UBSan gates. The same inputs and output bytes are portable
across CPU and HIP builds; no oracle code is linked into the application.
