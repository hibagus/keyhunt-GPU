# Current CPU modes

The main executable retains these original modes after C02. Support means the
mode exists and the stated small characterization checks pass; it does not
certify every flag combination or exact coverage. These legacy flags remain CPU
paths; C09 adds a separate [HIP xpoint subcommand](HIP_XPOINT.md).
C18 adds native [CUDA xpoint and BSGS subcommands](CUDA_BACKEND.md) with the same
exact range and local checkpoint contracts. CUDA does not accelerate the legacy
CPU mode flags listed below.

| Mode | Target file contents | Characterization |
| --- | --- | --- |
| `address` (Bitcoin default) | Base58 P2PKH addresses, one per line | Compressed, uncompressed, both, boundary and no-match cases |
| `address -c eth` | Ethereum hexadecimal addresses | Known scalar-1 address |
| `rmd160` | 40 hexadecimal digits (HASH160) per line | Compressed, uncompressed, both and no-match cases |
| `xpoint` | 64 hexadecimal digits (public point x coordinate) per line | Aligned/tail bounds, stride, no-match and >64-bit scalars |
| `bsgs` | Compressed or uncompressed hexadecimal public keys | Both encodings, interior match, no-match and boundary defects |
| `vanity` | Prefix from `-v`, or the old vanity target file format | Known compressed prefix and no-match case |
| `minikeys` | Target Bitcoin addresses, with optional 22-character `-C` base | Fixed public base finds the first expected valid minikey |
| `pub2rmd` | Not active in the main executable | Prints removal message and exits 0 |

## Address and HASH160

`-l compress`, `-l uncompress`, or `-l both` selects the Bitcoin public-key
encoding. `address` compares address-derived targets; `rmd160` takes the raw
20-byte hash in hexadecimal. The [usage quickstart](USAGE.md#a-finite-address-search)
uses existing solved-puzzle addresses. Ethereum is selected with `-c eth` in
address mode and uses its own hash/encoding path.

## Xpoint

This mode compares public point x coordinates. A shared x coordinate alone does
not distinguish the two possible y signs, so it is not a full public-key or
Bitcoin address check. C09 implements the separate `xpoint --backend hip`
subcommand with full 32-byte targets, exact half-open ranges and CPU-verified
results. Existing CPU stride and endomorphism behavior must not be assumed to map
to exact GPU coverage; those flags are not accepted by the HIP command.

## BSGS

C10 adds separate [portable table preparation and HIP lookup validation](BSGS_TABLES.md)
commands. C11 adds [bounded HIP BSGS range search](HIP_BSGS.md), with exact tails,
all-target completion and CPU verification. The legacy commands below continue
using their original CPU caches.

Baby-step giant-step search takes full public-key targets and trades table
memory for search work. `-n` must have an exact square root divisible by 1024;
`-k` controls table scaling. Use the [small validated example](USAGE.md#a-finite-bsgs-example)
before choosing larger tables. Different `-B` orders and `-R` change selection;
C01's boundary characterization concerns sequential BSGS.

`-S` saves/reuses table/filter files. These are caches, not durable computation
progress. C06 fixed the original miss at the tested range start. The old engine
can still report tail matches beyond the end. The optional GMP legacy parser also
rejects the tested valid uncompressed public key; that historical difference is
recorded in [C02](BUILD_MIGRATION.md#validation-and-observed-defects).

## Vanity and minikeys

Vanity search accepts `-v` address prefixes; it writes `VANITYKEYFOUND.txt`.
Minikeys use a different candidate space and validity check. Their fixed base
is incremented before testing, and a match does not end the search. The
characterization harness uses time limits and checks result files; there is no
finite scalar-range completion or durable checkpoint contract for this mode.

## Removed and unaudited behavior

The old README describes `pub2rmd` as experimental, but the main parser exits
immediately after announcing its removal. The optional legacy executable has
separate historical behavior; it is not a replacement GPU backend.

Endomorphism, random orders, special encodings and large table configurations
need dedicated correctness gates before being included in the new scheduler.
The [historical documentation](HISTORICAL_README.md) is retained for reference;
[current baseline findings](CPU_BASELINE.md) take precedence over old claims.
