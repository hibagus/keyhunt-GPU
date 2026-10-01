# C23: Ethereum address family

Status: implementation in progress. Bitcoin P2PKH/HASH160 acceptance remains in
[C23_VALIDATION.md](C23_VALIDATION.md). This pass implements the next family in
[the delivery order](GPU_REDESIGN_PLAN.md): Ethereum externally owned account
address derivation. Vanity, minikeys and other families remain pending.

## Contract and decisions

- `ethereum` is a separate native command and durable job mode. Targets are
  exactly 20 bytes. Scalar intervals remain exact, half-open subsets of `[1,n)`.
- Derive the secp256k1 public point, serialize big-endian `X || Y` (64 bytes,
  without the SEC1 prefix), hash with Keccak-256, and compare the final 20 bytes.
  Compressed keys and SHA3-256 are different inputs/algorithms and are rejected
  as Ethereum options. The retained CPU Keccak implementation independently
  verifies every GPU candidate.
- Accept 40 hexadecimal digits with an optional lowercase `0x` prefix. Uniform
  lowercase or uppercase is accepted for existing raw-address files; mixed case
  must pass ERC-55. Blank lines and CRLF are accepted, other whitespace, embedded
  NULs, invalid lengths, and nonhex characters are rejected. Output is lowercase
  `0x` plus 40 digits. There is no chain-specific checksum or ENS resolution.
- Sort/deduplicate binary targets; bind their identity to SHA-256 of
  `ethereum-v1\0` followed by concatenated 20-byte addresses. Input capitalization
  and prefix cannot change a job identity. Limit input to 1,048,576 targets.
- Use scalar work algorithm `DirectEthereumV1=3` and journal mode `Ethereum=4`.
  Configuration uses the existing 50-byte `khsearch` version-1 format with mode
  byte 4, zero table size and zero table checksum. This preserves older mode
  identities and journal schema. Unknown modes must fail closed.
- Each scalar can emit at most one canonical Ethereum target. There is no bound
  on the number of scalar preimages of a target. Overflow discards the entire
  attempt, credits no coverage, and replays smaller batches down to one scalar.
- Direct and four-scalar stepped HIP/CUDA kernels use the existing point
  arithmetic, with a separate portable Keccak primitive. Worker support requires
  an explicit `ethereum-v1` capability before grants can be allocated.

These encoding decisions follow the [Ethereum account description](https://ethereum.org/en/developers/docs/accounts/)
and [ERC-55](https://eips.ethereum.org/EIPS/eip-55). Keccak uses the 24-round
1600-bit permutation, a 136-byte rate and suffix `0x01`; standardized SHA3 uses a
different suffix. See the [Keccak specification summary](https://keccak.team/keccak_specs_summary.html)
and [PyCryptodome Keccak documentation](https://www.pycryptodome.org/src/hash/keccak).

## Acceptance gates

1. Canonical parsing, ERC-55 vectors, CPU derivation, identity isolation and
   invalid-input rejection, including sanitizers.
2. Portable and native Keccak results checked against a pinned independent
   PyCryptodome oracle, including padding boundaries and SHA3 discrimination.
3. Native HIP (MI300X) and CUDA (H200): direct/stepped, dense/sparse/no-match,
   arbitrary high scalars, carry/curve-order tails, maximum batch offsets,
   bounded overflow replay, ticket ownership and fault rejection.
4. Durable recovery, pause/restart, altered geometry, false receipts, retained
   results, coordinator capability negotiation and disconnected HTTPS operation.
5. Executable examples and frozen evidence under `docs/baselines/`. This family
   makes no throughput, scaling, tuning, MIG or partition certification claim.

Only public, deliberately small scalar fixtures are used for validation.
