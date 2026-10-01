# C23 exact BSGS dance acceptance

`--tile-order dance` is implemented for native BSGS, checkpoint runs and
supervised HIP/CUDA workers. It cycles low, high and middle per completed tile,
with a fixed midpoint and exact scalar coverage. C23 remains partial: additional
random traversal semantics and alternative minikey orders are still pending.

## Decisions and implementation

The midpoint is L + floor((H-L)/2), where L/H bound the initial missing intervals
for this invocation. A single split prevents tiles and work units from crossing
it. The middle front advances upward from that pivot, falling back to the lowest
remaining endpoint when the upper half is exhausted. It does not repeatedly
choose the median of remaining work. That alternative would introduce many
interior holes on huge ranges; this policy needs only the original gaps plus
four planner entries and at most three active work units.

The shared host planner uses an ordered interval index, permitting middle lookup
without scanning all gaps. Endpoints and products use checked UInt256 arithmetic.
Forward, reverse and both-ends retain their existing sequences, checked against
their independent reference model. GPU arithmetic inside every tile stays forward.
This exact policy is distinct from legacy `-B dance`, whose random interior
samples can overlap without shrinking the remaining range.

A phase advances only after every target group succeeds. Overflow retries retain
the same tile. Incomplete groups can save verified matches, but no scalar coverage;
replay deduplicates those results. Adaptive work units may interleave, with each
unit charged only its own execution time, excluding pauses and other units.

Restart begins low and recalculates the pivot from the saved complement. The
submission sequence may change while the certified scalar union remains exact.
No traversal state is persisted: job/configuration/table identities, schema 7,
protocol, capabilities and receipts stay unchanged. Old BSGS workers can process
the same grants forward. Scalar `--order` and coordinator block claims remain
separate. Non-BSGS runners and `checkpoint create` reject tile-order overrides.
See [the contract and executable example](C23_BSGS_DANCE.md).

## Recorded validation

Both stacks exposed eight devices: MI300X SPX/NPS1 and NVIDIA H200. Native tests
cover both BSGS kernels, with group-8 spot checks on every visible ordinal.

| Gate | Observed coverage |
| --- | --- |
| CPU release | All 127 full-suite gates passed |
| Dance oracle | 1,071 independent integer sequence cases, tiny exhaustive ranges, fragmented gaps, changing work sizes, fixed-pivot boundaries, meeting fronts, wide products and curve-order tails; observed three active units and at most one extra missing gap |
| Existing oracles | 3,164 forward/reverse/both-ends sequences and 5,535 tile bounds/reconstruction cases passed |
| Dance native | 22 cases per backend: both kernels, high origins, curve-order limits, single scalars, tails, misses, dense overflow, SEC1 normalization, eight ordinals and maximum-giant tails |
| Dance checkpoints | Nine cases per backend: two dense overflow/group cases and seven killed-process policy transitions, changing geometry with exact coverage and deduplicated public-key results |
| Partial middle replay | Sixteen host combinations: lost subgroup acknowledgment in the first or middle tile, restart in any of four policies, fixed/adaptive units, pause and completed-grant retry |
| Pause/visibility | Dance → reverse → dance, socket/signal pause, partial-target matches, backup/restore quarantine and changed visible ordinals on both backends |
| Fragmented coordinator recovery | Three accepted islands and four missing gaps, all four policies with fixed/adaptive units, old two-capability worker transfer, prior-owner results and identical lost-upload retry |
| Dance workers | HTTPS and disconnected-file transport on both backends; two grants each, one prepared executor, exact work-unit union, pivot clipping and matching local/server results |
| Previous traversal regressions | 56 native cases, 15 checkpoint cases, both pause/restore policies, four worker cases and two public examples per backend |
| Executor regressions | Existing BSGS executor and failure-injection gates passed on both backends |
| Sanitizers | Seven focused ASan/UBSan gates: three oracles, three storage recovery suites and fragmented coordinator recovery |
| Dance example | CPU preparation plus full HIP/CUDA search, checkpoint, completed-grant forward retry and journal integrity checks |

Each GPU acceptance run passed all 24 selected gates. The
[manifest](baselines/C23_BSGS_DANCE_VALIDATION.json) records source commits,
binary hashes, compiler/build settings and artifact checksums.
[Algorithms](baselines/C23_BSGS_DANCE_ALGORITHMS.json),
[recovery](baselines/C23_BSGS_DANCE_RECOVERY.json),
[workers](baselines/C23_BSGS_DANCE_WORKERS.json),
[examples](baselines/C23_BSGS_DANCE_EXAMPLES.json) and
[raw logs](baselines/C23_BSGS_DANCE_LOGS.tar.gz) retain the evidence.
No enrollment credentials or journal databases are archived.

## Findings and limits

The first sanitizer build used a cached Makefile with a list containing a newly
introduced target. Although the first target triggered CMake regeneration, the
original make invocation could not resolve that new target. Explicit configure
and rebuild fixed the invocation; all seven sanitizer checks then passed without
a production change. The initial build log and final configure/build/test logs
are retained.

The middle-tile recovery fixture saves both endpoint tiles before losing an
acknowledgment for a partial middle target group. It verifies that the saved
endpoints are never searched again and the middle matches survive replay exactly
once. The fragmented coordinator fixture uses truthful accepted receipts and
public recovery/sync APIs, including the previous owner's server-side results.

This is a host traversal feature with no new device arithmetic, table format,
calibration or throughput claim. Hardware results do not establish every kernel
on every ordinal, fleet scaling, or new GPU partition configurations. Sanitizer
coverage is restricted to the seven listed host gates. Legacy random dance is
not reproduced; random search semantics remain a separate C23 decision.

## Reproduction

Use [BUILD.md](BUILD.md) with coordinator/HTTPS support and the existing Apache
test fixture. Set the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness and put journals outside Git checkouts.

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure \
  -R '^(bsgs_(dance|both_ends|reverse)_.*|storage_bsgs_.*|coordinator_bsgs_.*|(hip|cuda)_bsgs_search(_failures)?)$'
```

The full CPU suite and focused sanitizer invocation supply the host coverage.
`tests/integration/bsgs_reverse_examples.py --dance` executes the marked blocks
directly from [the public example](C23_BSGS_DANCE.md#executable-public-example).
