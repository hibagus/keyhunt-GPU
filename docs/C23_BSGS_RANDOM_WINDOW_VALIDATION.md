# C23 seeded BSGS random-window acceptance

`--tile-order random-window` adds exact seeded traversal to native BSGS,
checkpoint runs and supervised HIP/CUDA workers. It shuffles a bounded window of
tiles, exhausts it, then advances to the next window. C23 remains partial:
other families' random traversal still needs separate implementation and parity
checks. See [the contract and executable example](C23_BSGS_RANDOM_WINDOW.md).

## Decisions and implementation

The window contains the next W tiles from the lowest missing endpoints. Each
tile is clipped at its saved gap, so no tile crosses accepted coverage. A window
may span several gaps. W is selected by `--tile-window 1..256` (default 64);
`--tile-seed HEX` is a 256-bit seed (default zero). Both overrides require
random-window. Window size one is sequential. This policy randomizes locally
within ascending windows; it is not globally uniform selection or legacy
sampling with replacement.

At window creation, the planner samples the current adaptive work span and
partitions the tiles into contiguous accounting units. The tile count per unit
is rounded down, with a minimum of one; gap and window tails clip the unit.
Shuffling crosses those ownership boundaries. Each unit starts at its first
submitted tile and completes at its last, retaining only its own execution time.
Sizing observations affect the next window. This preserves meaningful shuffling
with fixed one-tile units and bounds active ownership by W.

The shuffle uses descending Fisher-Yates with a specified SHA-256 stream over a
domain tag, big-endian seed and big-endian counter. Masked rejection draws avoid
modulo bias and standard-library differences. Counter overflow or 1,024 failed
draws aborts rather than changing selection rules. These controls specify order;
they do not claim cryptographic security or increased throughput.

No new window opens until all targets of its final tile complete. Overflow
replays the current tile without another draw. Partial target groups may save
verified matches but cannot certify scalar coverage. Restart reconstructs the
missing complement and resets the stream. Seed, window, geometry and policy can
change while exact receipts and deduplicated results remain authoritative.

There are no job/configuration/table identity, schema 7, protocol or capability
changes. Earlier BSGS workers can resume the same grants forward. Scalar
`--order`, minikey `--ordinal-order` and coordinator block claims are separate.
Native start/summary, checkpoint summary and worker completion records expose
`tile_seed` and `tile_window` for this policy.

## Recorded validation

The hardware corpus uses eight MI300X SPX/NPS1 devices and eight NVIDIA H200
devices. Both BSGS kernels run, with group-8 spot checks on every visible ordinal.

| Gate | Observed coverage |
| --- | --- |
| CPU release | All 153 full-suite gates passed |
| Independent random-window oracle | 1,364 cases: tiny domains, fragmented gaps, seed endpoints, windows 1..256, changing work sizes, wide products and curve-order tails; 12,945 rejected draws exercised; observed 71 active units and 68 additional accepted fragments, within each window's bound |
| Earlier oracles | 1,071 dance sequences, 3,164 forward/reverse/both-ends sequences and 5,535 tile bounds/reconstruction cases passed |
| Random-window native | 33 cases per backend: both kernels, high origins, curve-order limits, single scalars, tails, misses, dense overflow, SEC1 normalization, all eight ordinals, maximum-giant tails, windows 1/2/3/4/64/256 and default seed/window |
| Random-window checkpoints | 11 cases per backend: two dense overflow/group cases and nine killed-process transitions to/from all five orders; changed seed/window and giant geometry, exact coverage and deduplicated results |
| Partial subgroup replay | 20 host combinations: lost acknowledgment on the first or second shuffled tile, restart in all five orders, fixed/adaptive sizing, pause and completed-grant retry with another seed/window |
| Pause/visibility | Random-window → reverse → random-window, socket/signal pause, partial-target matches, backup/restore quarantine and changed visible ordinals on both backends |
| Fragmented coordinator recovery | Two new random-window fixed/adaptive cases plus eight prior order/sizing combinations: three accepted islands, four missing gaps, an older worker's capabilities/results, exact remaining coverage and byte-identical lost-upload retry |
| Random-window workers | HTTPS/group-1 and disconnected-file/group-8 on each backend; two grants per case, one prepared executor, exact work-unit union and window clipping, matching local/server results and duplicate import |
| Earlier traversal regressions | 78 native cases, 24 checkpoint cases, three pause/restore cases, six worker cases and three executable examples per backend |
| Executor regressions | Existing BSGS executor and failure-injection gates passed on both backends |
| Sanitizers | All 11 focused ASan/UBSan gates: core BSGS contract, four oracles, four storage recovery suites and two fragmented coordinator recovery policies |
| Public random-window example | CPU preparation plus full HIP/CUDA native search, checkpoint, completed-grant forward retry and journal integrity checks |

Each GPU acceptance run passed all 32 selected gates. The
[manifest](baselines/C23_BSGS_RANDOM_WINDOW_VALIDATION.json) records tested source
commits, binary hashes, compiler/build settings and artifact checksums.
[Algorithms](baselines/C23_BSGS_RANDOM_WINDOW_ALGORITHMS.json),
[recovery](baselines/C23_BSGS_RANDOM_WINDOW_RECOVERY.json),
[workers](baselines/C23_BSGS_RANDOM_WINDOW_WORKERS.json),
[examples](baselines/C23_BSGS_RANDOM_WINDOW_EXAMPLES.json) and
[raw logs](baselines/C23_BSGS_RANDOM_WINDOW_LOGS.tar.gz) retain the evidence.
Only public fixtures are archived, without enrollment credentials or journal
databases. Executable example reports bind the exact document SHA-256.

## Findings and limits

The independent Python model forms geometric tiles before assigning owners;
production forms work partitions and then tiles. Their complete submitted
sequences and first/last ownership flags agree. Checking the sorted accepted
union separately catches overlap and measures fragmentation without reproducing
the production interval map. Huge domains use bounded prefixes, not enumeration.

The partial-group fixture deliberately interrupts a shuffled interior tile.
When the earlier tile is already certified, recovery must skip its 14 scalars
and cover exactly the other 83. It also preserves the interrupted tile's saved
matches exactly once. Restarting a finished grant with another seed/window
submits zero batches. Coordinator recovery uses truthful accepted receipts and
public recovery/sync APIs, including the prior owner's server-side results.

This is a host traversal feature: no device arithmetic, table format, calibration
or throughput change is claimed. Hardware checks do not establish every kernel
on every ordinal, concurrent fleet scaling or new GPU partition configurations.
Sanitizers cover the eleven listed host gates, not device code or the whole
application. The observed fragmentation/ownership maxima describe this corpus;
the contract allows up to W additional fragments and W active owners. Other
families' random traversal remains pending.

## Reproduction

Use [BUILD.md](BUILD.md) with coordinator/HTTPS support and the existing Apache
test fixture. Set the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness and put journals outside Git checkouts. Configure
the chosen CPU/HIP/CUDA build as documented before these commands:

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure \
  -R '^(bsgs_(random_window|dance|both_ends|reverse)_.*|storage_bsgs_.*|coordinator_bsgs_.*|(hip|cuda)_bsgs_search(_failures)?)$'
```

For the configured ASan/UBSan CPU build:

```sh
TMPDIR=/var/tmp ctest --test-dir build/coordinator-sanitizers --output-on-failure \
  -R '^(bsgs_search_contract|bsgs_reverse_tile_oracle|bsgs_(both_ends|dance|random_window)_plan_oracle|storage_bsgs_(reverse|both_ends|dance|random_window)|coordinator_bsgs_(reverse|random_window))$'
```

Run the full CPU release suite without `-R`. The example gate executes the marked
blocks directly from [the public example](C23_BSGS_RANDOM_WINDOW.md#executable-public-example)
using `tests/integration/bsgs_reverse_examples.py --random-window`.
