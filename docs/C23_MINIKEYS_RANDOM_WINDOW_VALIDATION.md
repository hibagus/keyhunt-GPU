# C23 seeded minikey random-window acceptance

Native minikey search, checkpoint runs and supervised HIP/CUDA workers support
`--ordinal-order random-window` for 22- and 30-character minikeys. The policy
shuffles bounded ordinal tiles and preserves exact coverage through overflow,
restart and worker recovery. All five supported orders can recover the same
job. C23 remains partial: scalar families' alternative traversal still requires
implementation and parity gates. See [the contract and executable example](C23_MINIKEYS_RANDOM_WINDOW.md).

## Decisions and implementation

A window contains the next W tiles from the lowest missing endpoints. Its tile
width is the batch limit when that window opens, clipped at saved gap boundaries.
`--ordinal-window 1..256` sets W (default 64); `--ordinal-seed HEX` supplies a
256-bit seed (default zero). Both overrides require random-window. A window may
span gaps, but each tile and work unit stays inside one gap. Windows advance in
ascending order. This policy randomizes locally rather than selecting globally
uniform candidates or sampling with replacement; window size one is sequential.

Tile boundaries remain fixed until the window finishes. An overflow attempt
replans the same unaccepted tile suffix using a smaller batch. Each accepted
sub-batch consumes exactly its own interval, and that tile finishes before the
next shuffled tile is selected. Growing batches are clipped to its endpoint.
The next window samples the current batch limit. Every tile uses the existing
forward minikey lane mapping and GPU arithmetic.

Contiguous work units are partitioned before shuffling. Their tile count is
rounded down from the sampled work span, with a minimum of one; gap/window tails
and the UInt64 work-unit limit clip reservations. Each owner begins on its first
submitted tile and finishes on the last accepted sub-batch of its final tile.
Overflow announces no duplicate reservation. Each owner records its own execution
and replay time, excluding pauses and other owners. Adaptive changes apply to
the next window. At most W tile records and W active owners remain beyond the
input gaps, with at most W extra accepted fragments while completing a window.

Descending Fisher-Yates uses the specified SHA-256 domain/seed/counter stream
and masked rejection draws. The byte order, counter advancement and rejection
rule are explicit, avoiding library-dependent shuffles and modulo bias. Counter
overflow or 1,024 rejected attempts fails closed. The seed controls order and
makes no security or throughput promise.

Restart rebuilds the exact missing complement and resets the stream. Seed,
window, sizing, order and device may change. Canonical receipts and deduplicated
results define recovery; uninterrupted submission order need not be reproduced.
Job/configuration/target identities, schema 7, protocol, capabilities and
`minikey-ordinal-v1` stay unchanged. Earlier minikey-capable workers can resume
forward. Scalar order, BSGS tile options and coordinator block claims remain
separate. Start/summary and worker completion records expose the selected seed
and window. Creation and non-minikey runners reject these options.

## Recorded validation

| Gate | Observed coverage |
| --- | --- |
| CPU release | All 160 full-suite gates passed |
| Independent random-window oracle | 1,189 cases across both lengths, fragmented gaps, tiny/full domains, seed endpoints, window bounds, changing work/batch sizes and UInt64 limits; 13,152 unaccepted attempts and 15,950 rejected shuffle draws; observed 73 active owners and 70 extra accepted fragments, within their window bounds |
| Earlier order oracle | 2,495 independent sequence/candidate cases across forward, reverse, both-ends and dance; existing 144 planner invariant scenarios retained |
| Primitive oracle | 1,781 cases each for portable, HIP and CUDA implementations |
| Random-window native | 90 cases per backend: both lengths and all encodings, address/HASH160 targets, base-58/limb carries, endpoints, rejected candidates, overflow, tails, windows 1/2/3/4/64/256, default settings and spots on all eight ordinals |
| Large candidate set | Every admitted candidate in 1,048,576 ordinals is targeted: 4,193 valid 22-character and 4,261 valid 30-character candidates in each policy |
| Executor/fault gates | Existing direction-switch, retained-output/capacity/ownership checks and 29 allocation/runtime/corruption boundaries per direction and backend |
| Random-window checkpoints | 24 cases per backend: six overflow/rejected/last-ordinal cases and 18 killed restarts, across both lengths and all five orders; changed seed/window and batch geometry |
| Interrupted host recovery | 40 combinations: both lengths × all five restart orders × fixed/adaptive sizing × two acknowledgment-loss points; real overflow, exact saved union, pause and completed-grant retry |
| Fragmented workers | Four new random-window cases, both lengths × fixed/adaptive sizing: three accepted islands, old six-capability owner compatibility, prior results and byte-identical lost-upload retry |
| Pause/visibility | Random-window → reverse → random-window for both lengths, plus every prior policy; socket/signal pause, backup/restore quarantine and changed visible ordinals |
| Supervised workers | Four cases per backend and policy: both lengths × HTTPS/disconnected file exchange; two grants, one prepared executor, exact reservation union, window clipping, matching local/server results and duplicate import |
| Earlier-policy regressions | 296 native cases, 56 checkpoint cases, eight pause/restore cases, twelve worker cases and three executable examples per backend |
| Sanitizers | 14 distinct focused host ASan/UBSan gates passed; the new recovery fixture required a longer timeout and focused rerun |
| Public random-window example | CPU preparation plus full HIP/CUDA native search, checkpoint, completed-grant reverse retry and journal integrity checks; 101 ordinals in six batches |

Both GPU builds passed all 40 selected gates. The hosts exposed eight MI300X
SPX/NPS1 and eight H200 devices. Device spots alternate lengths; these checks do
not cover every length/device combination or establish concurrent fleet scaling.

The [manifest](baselines/C23_MINIKEYS_RANDOM_WINDOW_VALIDATION.json) records source
phases, binary hashes, compiler/build settings, gate counts and artifact checksums.
[Algorithms](baselines/C23_MINIKEYS_RANDOM_WINDOW_ALGORITHMS.json),
[recovery](baselines/C23_MINIKEYS_RANDOM_WINDOW_RECOVERY.json),
[workers](baselines/C23_MINIKEYS_RANDOM_WINDOW_WORKERS.json),
[examples](baselines/C23_MINIKEYS_RANDOM_WINDOW_EXAMPLES.json) and
[raw logs](baselines/C23_MINIKEYS_RANDOM_WINDOW_LOGS.tar.gz) retain the observations.
Executable example reports bind their exact document SHA-256. Only public
fixtures are archived, without enrollment credentials or journal databases.

## Findings and limits

The initial sanitizer run passed thirteen gates; the new forty-case storage
fixture exceeded its 180-second deadline. Its CTest allowance was increased to
600 seconds, retaining the full corpus. The unchanged binary passed its focused
rerun in 502.53 seconds. No executable code was changed or rebuilt for that retry; binary hashes,
the original timeout, the CMake-only adjustment and the retry log are retained.
Builds use `569ea1d`; `0f6127d` changes only this timeout. Hardware acceptance
uses the same implementation and test binaries.

The independent Python model forms geometric tiles before assigning owners;
production forms work reservations before tiles. Their sequences and ownership
flags agree through repeated unaccepted plans and changing sizes. A separate
sorted accepted union checks overlap and fragmentation without copying the
production interval map. Huge-domain cases inspect bounded prefixes.

The storage fixture targets every admitted candidate in a 4,097-ordinal range,
forcing real overflow in the first shuffled tile. It loses the acknowledgment
after the first accepted sub-batch, or after that tile completes and the next
shuffled tile accepts work. Recovery removes each missing ordinal exactly once
and preserves both encoding relations. A completed grant retried with another
seed and window executes zero batches. Fragmented coordinator recovery uses
truthful receipts and public recovery/sync APIs, including prior-owner results.

This is a host traversal change with no new device arithmetic, table format,
calibration or throughput claim. The observed ownership/fragmentation maxima
describe this corpus, not the full allowed bound of W. The sanitizer claim is
limited to the fourteen listed host gates, not device code or the full program.
Scalar families' alternative traversal remains pending.

## Reproduction

Use [BUILD.md](BUILD.md), coordinator/HTTPS support and the existing Apache test
fixture. Configure the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness. Keep journals outside Git checkouts. After
configuring the chosen CPU/HIP/CUDA build:

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure \
  -R '^(minikey.*|portable_minikey_oracle|storage_minikey.*|checkpoint_minikey.*|coordinator_minikey.*|(hip|cuda)_minikey.*)$'
```

For the configured ASan/UBSan CPU build:

```sh
TMPDIR=/var/tmp ctest --test-dir build/coordinator-sanitizers --output-on-failure \
  -R '^(minikey_search_contract|minikey_order_oracle|minikey_random_window_oracle|storage_minikey.*|coordinator_minikeys|coordinator_minikey_(reverse|both_ends|dance|random_window)|work_unit)$'
```

Run the full CPU release suite without `-R`.
`tests/integration/minikey_random_window_examples.py` executes the marked
[public example](C23_MINIKEYS_RANDOM_WINDOW.md#executable-public-example) verbatim.
