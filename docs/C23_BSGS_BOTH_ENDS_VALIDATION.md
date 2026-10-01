# C23 both-ends BSGS acceptance

`--tile-order both-ends` is implemented for native BSGS, checkpoint runs and
supervised HIP/CUDA workers. It starts at the lowest uncovered tile and alternates
high/low after each completed tile. C23 remains partial: dance, additional random
traversal semantics and alternative minikey orders remain pending.

## Decisions and implementation

A shared host planner selects exact scalar tiles over sorted, disjoint missing
intervals. It reserves contiguous adaptive work units at the two outer ends and
alternates actual tiles inside them. When the fronts meet, both ends can consume
one remaining unit. The queue holds at most the initial gaps plus two active
units. Tile width is bounded by m×giants, remaining coverage and its work-unit
boundary; all endpoints use checked UInt256 arithmetic.

Target subsets and overflow retries complete the same tile before selection
moves to the other end. Only a fully verified all-target tile receives scalar
coverage. Matches from incomplete groups persist and deduplicate after replay.
Each work unit adapts from its own active time, excluding pause and opposite-end
execution. Interleaved units can therefore take longer in wall time than their
active-time estimate without changing pause or checkpoint boundaries.

A restart starts low again on the exact saved complement. No traversal counter
is persisted. This is execution policy: job/configuration/table identity, schema
7, receipts, protocol, capabilities and actual scalar result coordinates stay
unchanged. Older workers can still execute the same BSGS jobs forward. Scalar
`--order` and coordinator block claims remain separate. Non-BSGS runners and
`checkpoint create` reject tile-order overrides.

The native option is intentionally named `both-ends`: legacy `-B both` chooses
an end randomly, while this new exact policy alternates deterministically.
Forward remains the default, reverse retains its meaning, and GPU arithmetic
inside each tile is unchanged. See [contract and example](C23_BSGS_BOTH_ENDS.md).

## Recorded validation

Both stacks exposed eight devices: MI300X SPX/NPS1 and NVIDIA H200. Native cases
cover both BSGS kernels; group-8 spot checks cover each visible ordinal. This does
not establish every kernel/device combination, fleet scaling or new partitions.

| Gate | Observed coverage |
| --- | --- |
| CPU release | 120/120 full-suite gates; the subsequently added documented preparation example also passes |
| Sequence oracle | 3,164 independent integer cases across forward/reverse/both-ends, tiny exhaustive walks, fragmented gaps, changing work sizes, active-unit meeting, wide products and invalid inputs |
| Bounds regression | Existing 5,535-case tile bounds and final-baby reconstruction oracle passes |
| Both-ends native | 22 cases per backend: both kernels, low/high/near-order origins, single-scalar and short-middle tails, misses, dense overflow, SEC1 normalization, eight ordinals and maximum-giant tails |
| Both-ends checkpoints | Seven cases per backend: two dense grouping/overflow cases and five killed-process transitions into/out of both-ends, with changed geometry and exact result deduplication |
| Pause/visibility | Both-ends → reverse → both-ends, socket/signal pause, partial-target matches, backup/restore quarantine and changed visible ordinals on both backends |
| Fragmented recovery | Three coverage islands and four missing gaps, all three orders with fixed/adaptive units, old two-capability worker handoff, prior-owner results and byte-identical lost-upload retry |
| Both-ends workers | HTTPS and disconnected-file cases on each backend, two grants per case, one prepared executor, exact work-unit union and identical local/server results |
| Forward/reverse regressions | 34 native and eight checkpoint cases per backend, reverse pause/restore, both worker transports and the reverse documented example |
| Executor regressions | Existing BSGS native executor and failure-injection gates on both backends |
| Sanitizers | Five focused ASan/UBSan gates: sequence/bounds oracles, both-ends/reverse storage recovery and fragmented coordinator recovery |
| Documentation | CPU preparation plus full HIP/CUDA both-ends search, checkpoint, forward completed-grant retry and journal integrity checks |

Each GPU acceptance invocation passed all 16 selected gates, followed by the new
public example. The [manifest](baselines/C23_BSGS_BOTH_ENDS_VALIDATION.json) records
source commits, binary hashes, compiler/build settings and artifact checksums.
[Algorithms](baselines/C23_BSGS_BOTH_ENDS_ALGORITHMS.json),
[recovery](baselines/C23_BSGS_BOTH_ENDS_RECOVERY.json),
[workers](baselines/C23_BSGS_BOTH_ENDS_WORKERS.json),
[examples](baselines/C23_BSGS_BOTH_ENDS_EXAMPLES.json) and
[raw logs](baselines/C23_BSGS_BOTH_ENDS_LOGS.tar.gz) retain the observations.
No enrollment credentials or journal databases are archived.

## Findings and limits

The first independent Python ownership model omitted opposite-end reservations
and clipping to the remaining middle span. For a three-scalar interval and
two-scalar tiles, its second tile overlapped the first; production correctly
returned a one-scalar tail. Adding the missing reference bounds yielded 3,164
matching sequences. The initial failure and passing suites are preserved.
No production arithmetic fix was needed for that reference-model error.

The storage test deliberately forces adaptation to put multiple tiles into one
unit, then verifies actual endpoint alternation, partial-target acknowledgment
loss, pause and all-order restart. The coordinator test imports real accepted
receipts and recovers an expired grant through the existing public protocol;
no private journal APIs were exposed for fixtures.

This slice adds host traversal policy, with no new kernel, table format,
calibration, throughput claim, public deployment or random-search guarantee.
The sanitizer claim covers only the five listed host gates. A resumed submission
sequence can differ from uninterrupted execution; its certified scalar union
and deduplicated results remain exact.

## Reproduction

Use [BUILD.md](BUILD.md), coordinator/HTTPS support and the existing Apache test
fixture. Set the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness and use a journal parent outside Git checkouts.

```sh
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure \
  -R '^(bsgs_(both_ends|reverse)_.*|storage_bsgs_.*|coordinator_bsgs_.*|(hip|cuda)_bsgs_search(_failures)?)$'
```

The full CPU suite and focused sanitizer invocation provide the host coverage.
`tests/integration/bsgs_reverse_examples.py --both-ends` executes the marked
blocks directly from the [public example](C23_BSGS_BOTH_ENDS.md#executable-public-example).
