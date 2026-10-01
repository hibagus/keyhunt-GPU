# C23: Bitcoin P2PKH vanity prefixes

Status: implementation in progress. This family follows the accepted
[Bitcoin HASH160](C23_VALIDATION.md) and [Ethereum](C23_ETHEREUM_VALIDATION.md)
paths. Minikeys and other candidate mappings remain pending.

## Contract and decisions

- Native `vanity --backend hip|cuda` takes exact half-open scalar intervals and
  a file of case-sensitive Bitcoin mainnet P2PKH Base58 prefixes. Prefixes must
  begin with `1`, contain only Bitcoin Base58 characters and have 1..34 characters.
  Blank lines/CRLF are accepted; other whitespace, NULs and oversized lines fail.
  A syntactically valid prefix need not have a possible address completion.
- `--encoding compressed|uncompressed|both` defaults to both. The complete
  Base58Check address is generated before matching; checksum characters and
  leading zeroes participate. No case folding, regular expressions, suffixes,
  Ethereum, P2SH, witness or testnet prefixes are implied. Legacy `-m vanity`
  retains its CPU implementation and existing flags.
- Every matching prefix/encoding relation is returned. Overlapping `1`, `1B`
  and `1Bg` are separate results, including when they match the same scalar.
  Duplicate lines deduplicate. Searches exhaust the range, not stop at first hit.
- Canonical targets are 36 bytes: encoding tag 1/2, one-byte prefix length,
  ASCII prefix, then zero padding through byte 35. Sorted unique targets bind
  SHA-256 of `vanity-p2pkh-v1\0` plus these bytes. Limit expanded input to 4096
  targets. Work algorithm `DirectVanityV1=4`, journal mode `Vanity=5`, and the
  existing 50-byte configuration use zero table size/checksum, preserving schema 7.
- At most one prefix of each length matches an address. The conservative output
  bound is the sum of distinct prefix lengths for each enabled encoding (at most
  68), not the number of targets or an assumed bound on hash preimages. Capacity
  must fit this bound for one scalar. Overflow discards every candidate and credits
  no coverage; replay shrinks by this bound. GPU length masks skip absent lengths.
- Independent CPU verification uses retained SHA/RIPEMD and libbase58; portable
  GPU code uses the already validated hash primitives and its own fixed-storage
  conversion. Tests use Python integer Base58 plus hashlib/OpenSSL and pinned
  libsecp256k1 fixtures. No approximate HASH160 interval matching is credited.
- Durable checkpoints retain `(scalar, canonical target)` relations and reuse the
  scalar owner's fencing, pause and exact complement rules. Workers require an
  explicit `vanity-v1` capability before grants/renewals/updates/receipt replay.

The address construction follows the [Bitcoin developer reference](https://developer.bitcoin.org/reference/transactions.html#address-conversion)
and [Bitcoin Core Base58 implementation](https://github.com/bitcoin/bitcoin/blob/master/src/base58.cpp).
Full-address serialization avoids coupling coverage to the legacy CPU vanity
engine's approximate hash intervals and prefix-length restrictions.

## Acceptance gates

Canonical prefix parsing/encoding identity; CPU verification and sanitizer gates;
independent portable/native Base58Check vectors including leading zeroes; direct
and stepped HIP/CUDA searches with overlapping prefixes, dense overflow replay,
curve-order/carry tails, both encodings and all visible ordinals; checkpoint
recovery, pause/restart, HTTPS and disconnected workers; executable examples and
frozen evidence. Only public scalar fixtures are used. No throughput, scaling,
calibrated block-width or partition/MIG certification claim accompanies this slice.
