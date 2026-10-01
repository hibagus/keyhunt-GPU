# Modes and backend support

| Interface / mode | CPU build | HIP (MI300X) | CUDA (H200) |
| --- | --- | --- | --- |
| Legacy `-m address`, `rmd160`, `vanity`, `minikeys`, Ethereum | Characterized CPU paths, limitations below | Same CPU paths; no GPU acceleration | Same CPU paths; no GPU acceleration |
| Legacy `-m xpoint`, `-m bsgs` | Characterized CPU paths, range/stride limitations | Same CPU paths | Same CPU paths |
| Exact `xpoint --backend …` | Explicitly rejected | Validated | Validated |
| Exact `bsgs --backend …` | Explicitly rejected | Validated | Validated |
| Exact `hash160` / Bitcoin P2PKH `address --backend …` | Explicitly rejected | Validated | Validated |
| Exact `ethereum --backend …` | Explicitly rejected | Validated | Validated |
| Exact `vanity --backend …` | Explicitly rejected | Validated | Validated |
| Explicit six-member `--endomorphism orbit` (scalar families) | CPU job preparation only | Validated | Validated |
| Exact scalar `--stride HEX` and `--order forward\|reverse` (xpoint/HASH160/address/Ethereum/vanity) | Explicitly rejected; CPU job preparation supported | Validated | Validated |
| Exact `minikeys --backend … --length 22\|30` | Explicitly rejected; CPU inspect supported | Validated | Validated |
| `bsgs-table build` / `inspect`, state, job creation and result inspection | Supported | CPU operations | CPU operations |
| `checkpoint run --backend …` | Explicitly rejected | Validated durable xpoint/BSGS/HASH160/Ethereum/vanity/minikeys | Validated durable xpoint/BSGS/HASH160/Ethereum/vanity/minikeys |
| Optional supervised workers | Configuration/sync/file exchange; no CPU search executor | Validated xpoint/BSGS/HASH160/Ethereum/vanity/minikeys owners | Validated xpoint/BSGS/HASH160/Ethereum/vanity/minikeys owners |

The [GPU quickstart](GPU_QUICKSTART.md) includes exact ranges and public fixtures.
The [build matrix](BUILD.md#validated-gpu-builds) identifies tested stacks and
partition limits. [C23's first family](C23_HASH160.md) adds exact raw HASH160 and
Bitcoin mainnet P2PKH inputs with explicit public-key encoding identity.
[Ethereum](C23_ETHEREUM_VALIDATION.md) adds Keccak addresses;
[vanity](C23_VANITY_VALIDATION.md) adds exact case-sensitive Bitcoin P2PKH prefixes.
[Minikeys 22/30](C23_MINIKEYS_VALIDATION.md) adds exact candidate ordinals and recovery. [Positive strides](C23_STRIDES.md) add exact scalar progressions with candidate-index recovery. [Reverse traversal](C23_REVERSE_VALIDATION.md) covers the same candidates in descending order with exact recovery. [Six-member orbit expansion](C23_ORBITS.md) adds explicit derived candidates; [reverse BSGS tiles](C23_BSGS_REVERSE.md) preserve exact scalar coverage; other mappings require separate parity gates. C22 adds
[manual offline assignment files](OFFLINE_ASSIGNMENTS.md) for disconnected workers
and connected couriers. HTTPS workers also retain valid leases and a durable
outbox between scheduled contacts.

<a id="current-cpu-modes"></a>

## Current CPU modes

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

The native `hash160` and `address` subcommands use `--encoding compressed`,
`uncompressed` or `both` (default). Native address input requires canonical
Bitcoin mainnet P2PKH Base58Check; P2SH, witness, testnet and Ethereum are rejected.
Full hashes and encoding tags bind checkpoint jobs. Both encodings can produce
separate verified matches at one scalar. [Runnable GPU examples](GPU_QUICKSTART.md)
cover raw hashes, addresses and durable restart. The legacy syntax below is unchanged.

`-l compress`, `-l uncompress`, or `-l both` selects the Bitcoin public-key
encoding. `address` compares address-derived targets; `rmd160` takes the raw
20-byte hash in hexadecimal. The [usage quickstart](USAGE.md#a-finite-address-search)
uses existing solved-puzzle addresses. Ethereum is selected with `-c eth` in
address mode and uses its own hash/encoding path.

## Ethereum addresses

Native `ethereum --backend hip|cuda` accepts full 20-byte addresses as 40 hex
digits, optionally prefixed with `0x`. Lowercase/uppercase raw input and valid
mixed-case ERC-55 input identify the same binary targets. The GPU derives and
hashes both affine coordinates with Keccak-256; CPU verification uses a separate
implementation. There is no compression flag. Exact intervals, bounded overflow
replay, durable checkpoints and authenticated/offline workers are supported.
See [contracts](C23_ETHEREUM.md), [acceptance](C23_ETHEREUM_VALIDATION.md) and
[executable examples](GPU_QUICKSTART.md). Legacy `-m address -c eth` retains its
characterized CPU behavior.

## Xpoint

This mode compares public point x coordinates. A shared x coordinate alone does
not distinguish the two possible y signs, so it is not a full public-key or
Bitcoin address check. C09 implements the separate `xpoint --backend hip`
subcommand with full 32-byte targets, exact half-open ranges and CPU-verified
results. Native `--stride HEX` visits exactly `begin + i*stride < end`; nonunit
strides checkpoint candidate indices. Legacy `-I` and endomorphism flags retain
their separate CPU behavior. `--order reverse` visits that same finite set from last to first and always checkpoints candidate indices, including stride one. BSGS and minikey enumeration reject this option. See [stride contracts](C23_STRIDES.md) and [reverse contracts](C23_REVERSE.md).

## BSGS

C10 adds separate [portable table preparation and HIP lookup validation](BSGS_TABLES.md)
commands. C11 adds [bounded HIP BSGS range search](HIP_BSGS.md), with exact tails,
all-target completion and CPU verification. The legacy commands below continue
using their original CPU caches.

C23 adds native/checkpoint/worker `--tile-order forward|reverse|both-ends|dance|random-window` for BSGS.
Reverse selects the highest uncovered scalar tile in each grant. It preserves
job identity and exact scalar receipts, so a restart may change direction.
Scalar `--order` and coordinator block claim policies remain separate options.
`both-ends` starts low and alternates actual low/high tiles; restart starts low on
remaining coverage. See [reverse](C23_BSGS_REVERSE.md) and
[both-ends](C23_BSGS_BOTH_ENDS.md) contracts and examples.
`dance` cycles low/high/middle; its middle front advances from a fixed midpoint
and falls back to low when that half is exhausted. Unlike legacy random dance,
this has bounded planner state and no repeat scalar tiles. See
[the exact policy and restart contract](C23_BSGS_DANCE.md).
`random-window` shuffles the next 1..256 tiles from the lowest missing endpoints,
then exhausts that window before advancing. `--tile-window` defaults to 64 and
`--tile-seed HEX` to zero; both require random-window. This is local randomization
within ascending windows. Work partitions are fixed per window; restart resets
the stream on the saved complement. See [contracts and example](C23_BSGS_RANDOM_WINDOW.md).

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

Additional mappings, random orders, special encodings and large table configurations
need dedicated correctness gates before being included in the new scheduler.
The [historical documentation](HISTORICAL_README.md) is retained for reference;
[current baseline findings](CPU_BASELINE.md) take precedence over old claims.

## Native Bitcoin vanity prefixes

`vanity --backend hip|cuda --targets FILE --range BEGIN:END` checks complete
Bitcoin mainnet P2PKH Base58Check addresses. Each nonblank line is a 1..34-character
case-sensitive Base58 prefix beginning with `1`. `--encoding` selects compressed,
uncompressed or both (default). Full-address targets and leading-zero prefixes
are accepted. Overlapping prefixes produce separate verified results at a scalar.
The exact half-open range is exhausted. See [contracts](C23_VANITY.md),
[quickstart](GPU_QUICKSTART.md) and [checkpoints](CHECKPOINTS.md#bitcoin-vanity-prefix-jobs).
Legacy `-m vanity` keeps its original CPU behavior and flags.

## Native minikey candidates

`minikeys --backend hip|cuda --length 22|30 --targets FILE --range BEGIN:END`
checks a finite interval of candidate ordinals. Targets default to Bitcoin mainnet
P2PKH addresses; `--input-format hash160` accepts raw hashes. Both public-key
encodings are checked by default. `minikeys inspect --key TEXT` runs on the CPU
and gives the exact ordinal for a candidate. Ordinal 1 maps to `S` followed by
all `1`s; every checksum-rejected candidate still counts toward coverage.

Only the direct kernel applies because adjacent candidates derive unrelated
private keys. Public records expose the candidate ordinal and derived scalar
separately. The legacy `-m minikeys` path remains unchanged. See the
[contract](C23_MINIKEYS.md), [quickstart](GPU_QUICKSTART.md) and
[checkpoint guide](CHECKPOINTS.md#minikey-ordinal-jobs).
`--ordinal-order reverse` selects descending minikey execution for native searches,
checkpoint runs and workers. It preserves actual ordinal receipts and can change
on restart; see [the reverse contract and example](C23_MINIKEYS_REVERSE.md).
`--ordinal-order both-ends` alternates low/high batches, starting low after each
restart. Overflow retains the current end; see [the both-ends contract](C23_MINIKEYS_BOTH_ENDS.md).
`--ordinal-order dance` cycles low/high/fixed-midpoint-forward per accepted batch.
The pivot is fixed within an invocation and rebuilt after restart; see
[the dance contract and example](C23_MINIKEYS_DANCE.md).
`--ordinal-order random-window` shuffles bounded ascending windows of tiles,
exhausting the current tile's suffix through smaller overflow batches before
advancing. `--ordinal-seed HEX` (256-bit, default zero) and
`--ordinal-window 1..256` (default 64) require this policy. Work and tile geometry
are fixed per window; restart resets the stream on the exact saved complement. See [the contract and
example](C23_MINIKEYS_RANDOM_WINDOW.md).


## Exact-range GLV scalar multiplication

The four native scalar families (`xpoint`, Bitcoin `hash160`/`address`,
`ethereum`, `vanity`) also accept `--kernel glv`. It uses GLV arithmetic
for the original candidate scalar without changing the candidate set. Forward,
positive-stride and reverse mappings keep the same coverage and job identities.
`stepped` remains the default. Related-key expansion is selected separately with
`--endomorphism orbit`. Other search mappings remain separate work; BSGS and
minikeys have no GLV kernel.
See [contracts and validation](C23_GLV.md).

## Related-key orbit expansion

`--endomorphism orbit` searches six derived private scalars per seed for xpoint,
HASH160/P2PKH, Ethereum and vanity. The range/stride bounds the seeds; derived
keys can lie outside it. Default `none` preserves existing coverage and IDs.
Reverse changes seed order within each variant. Results expose candidate index,
seed, variant and actual private scalar; overlapping seed orbits remain distinct
observations. All three kernels support this mapping. BSGS and minikeys reject
it. See [coverage limits and executable example](C23_ORBITS.md) and
[acceptance](C23_ORBITS_VALIDATION.md).

## Both-ends scalar batches

Scalar searches accept `--batch-order forward|both-ends` (default forward).
This applies to xpoint, Bitcoin address/HASH160, Ethereum and vanity in native
searches, checkpoint runs, `keyhunt-worker run-device` and the Python supervisor.
Both-ends alternates low/high missing batches after acceptance, starting low.
Overflow retries the same end. Each batch preserves the immutable `--order`,
stride and orbit mapping; high batches are clipped at orbit variant boundaries.
At most two adaptive work owners are active within a grant.

Restart can switch batch order and geometry over the exact saved complement.
Existing identities, receipts and capability requirements remain valid; compatible
older workers can continue in forward batch order. Creation, BSGS and minikeys
reject explicit `--batch-order` overrides, including forward. Mixed-mode workers
should omit this override. Execution summaries report `batch_order`.
See [contracts and an executable public example](C23_SCALAR_BOTH_ENDS.md) and
[HIP/H200 validation](C23_SCALAR_BOTH_ENDS_VALIDATION.md). Scalar dance and random
windows remain pending; this change makes no performance claim.
