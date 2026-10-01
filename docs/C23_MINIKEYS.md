# C23: exact minikey candidate search

Status: implementation in progress. The agreed scope is SHA-256 minikeys of
22 or 30 characters, both public-key encodings, and native HIP/CUDA recovery.

## Mapping and identity

- A candidate is `S` followed by exactly 21 or 29 characters from Bitcoin's
  ordered alphabet `123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`.
  The suffix is a fixed-width base-58 integer, most significant digit first.
- A **one-based ordinal** `o` represents suffix integer `o-1`. Ordinal 1 is
  `S` followed by all `1`s. The candidate space is `[1, 58^(length-1)+1)`;
  ranges and block widths use hexadecimal, half-open endpoints. No wrap,
  probabilistic skipping or increment-before-testing is allowed.
- Every candidate, including invalid check bytes, contributes one ordinal of
  tested coverage. `SHA256(candidate + "?")[0] == 0` admits a candidate;
  `SHA256(candidate)` is its private scalar. Zero or values outside `[1,n)` are
  rejected, without reduction modulo the curve order. Each admitted scalar is
  matched against Bitcoin P2PKH targets using the selected key encodings.
- `--length 22|30` is required when creating/searching a job. `--encoding
  compressed|uncompressed|both` defaults to both. `--input-format address|hash160`
  defaults to address. Equivalent address/HASH160 files have the same identity.
- Canonical targets have 22 bytes: length byte, encoding tag 1/2, 20 HASH160 bytes.
  All targets in one job have the same length. Sorted unique targets bind
  SHA-256 of `minikeys-v1\0` plus their bytes. Expanded input is limited to
  1,048,576 targets, subject to existing coordinator message bounds.
- Work algorithm `DirectMinikeysV1=5` and journal mode `Minikeys=6` distinguish
  ordinal search from private-scalar search. Configuration remains the existing
  50-byte mode encoding with zero table size/checksum. Length is bound by the
  canonical targets. The existing schema 7 and migration files remain unchanged.

The scheduler's bounded integer intervals can represent either domain: even the
30-character ordinal limit is below the curve order. Mode 6 gives the intervals
ordinal semantics. An explicit `ordinal_at` accessor prevents interpreting these
coordinates as private scalars. Internal receipt/SQLite fields historically named
`scalar` retain the ordinal for coverage and result identity in mode 6. Public
match records expose `ordinal`, candidate text and the **derived** `scalar`;
coverage output identifies `minikey-ordinal-v1`. Every retained relation is
`(ordinal, canonical target)`, so collisions do not discard separate candidates.

## Execution and recovery

The GPU directly maps each ordinal to its candidate, hashes the check byte, and
only derives a public point for admitted private scalars. Consecutive candidates
hash to unrelated private scalars, so there is no stepped point-walk kernel.
Only `--kernel direct` is accepted. GPU seed/power caches are unnecessary.

Each ordinal emits at most one match per enabled encoding. Capacity must fit
that bound, even though valid candidates are sparse on average. Overflow drops
the entire attempt and credits nothing; replay shrinks to a provably fitting
ordinal interval. Rejected candidates still count as tested work.

CPU verification independently reconstructs the candidate, rechecks validity,
derives the private scalar, and verifies the full encoding/HASH160 relation.
Journal owners reuse existing fences, pause controls and exact complement replay.
An explicit `minikeys-v1` capability is required before coordinator allocation,
renewal, updates and cached receipt replay. Device owners retain targets and
allocations across grants and run a minikey startup check on their selected GPU.

`minikeys inspect --key TEXT` is a CPU-only mapping helper. It reports length,
ordinal and validity; valid examples also show the derived private scalar.
Native search and durable execution require `--backend hip|cuda`. Legacy
`-m minikeys`, its custom alphabet/base and its increment-before-testing behavior
remain separate CPU paths. Length 26, PBKDF2, custom alphabets and random traversal
are outside this agreed slice. `pub2rmd` remains removed from the main parser.

## Sources and acceptance gates

The admission and scalar derivation follow the original format's
[Bitcoin developer description](https://developer.bitcoin.org/devguide/wallets.html#mini-private-key-format)
and [Casascius implementation](https://github.com/casascius/Bitcoin-Address-Utility).
Tests use public published examples, Python integer arithmetic/hashlib and pinned
libsecp256k1 as independent oracles. No wallet files are used.

Gates cover first/last ordinals, base-58 and integer-limb carries, both lengths,
invalid checks, immutable encoding/length identity, duplicate relations, forced
overflow, GPU ownership/faults, checkpoint death/restart, pause/visibility changes,
authenticated/offline workers and executable examples on MI300X and H200. No
throughput, calibration, multi-device scaling or partition/MIG claim is implied.
