# Bounded HIP BSGS search (C11)

## Mapping and targets

C11 consumes C10's validated [versioned baby table](BSGS_TABLES.md). For a
nonempty scalar tile `[a,b)`, baby entries represent `jG`, `0 <= j < m`.
For each canonical target `Q`, the search looks up `Q - aG - i*mG` for
`0 <= i < ceil((b-a)/m)`. A full-point match reconstructs `k = a + i*m + j`.
Only `a <= k < b` is eligible, and every emitted match must satisfy `kG == Q`
in the preserved CPU verifier. Absolute scalars, endpoints and reconstruction
products use checked 256-bit integer arithmetic, never scalar-field reduction.

The last giant step admits only `(b-a) mod m` babies when this is nonzero.
Infinity is a valid residual and matches baby zero. Both point signs are
preserved through full compressed-point lookup; matching an X coordinate alone
is insufficient. Every exact collision-list entry must be considered.

Target files contain finite compressed (66 hex digits) or uncompressed (130 hex
digits) SEC1 public keys, one per nonempty line. LF, CRLF and no final newline
are accepted. Coordinates must be canonical and on curve; infinity, hybrid
encodings, nonresidues, embedded NUL and overlong lines are rejected. Parsing is
bounded to 65,536 input points. Compressed and uncompressed duplicates normalize
to one uncompressed point; opposite signs remain distinct. Sorted full points
are hashed with the `bsgs-targets-v1` domain tag, fixing canonical target IDs.

`BsgsBatch` binds an interval and contiguous target subset to the complete target
digest and table checksum. A batch permits at most 64 targets and 1,048,576
*target giant steps* (`giants * target_count`). The table size remains fixed
across batches. `bsgs_tile` clips a checked `m * max_giants` width before adding
it to the start, avoiding overflow at the order endpoint. This separate BSGS
plan does not reuse the scheduler's `DirectXPointV1` scalar mapping.

The owner must finish every target subset before crediting a tile once. A match
does not imply that other targets or the rest of the interval have been searched.
All C11 receipts are volatile; checkpointed/durable coverage remains C12/C13.

## Initial host validation

The CPU contract test passes in release and focused ASan/UBSan builds. It checks
SEC1 normalization and malformed input, opposite signs, target identity, candidate
verification/corruption, final partial tiles, products above 64 bits and an
exclusive endpoint at the curve order. GPU execution, independent oracle cases
and performance evidence are recorded below as their acceptance gates complete.
