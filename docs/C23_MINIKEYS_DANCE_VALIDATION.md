# C23 minikey dance acceptance

Native minikey search, checkpoint runs and supervised HIP/CUDA workers support
`--ordinal-order dance` for 22- and 30-character minikeys. It cycles through low,
high and fixed-midpoint-forward batches while retaining exact canonical ordinal
coverage. All four supported ordinal orders can recover the same job. Additional
random traversal semantics remain pending in C23.

## Decisions and implementation

The shared `MinikeyBatchPlanner` computes P = L + floor((H-L)/2) from the initial
missing envelope and splits a crossing gap once. Phase zero selects global low,
phase one global high, and phase two the lowest missing endpoint at or above P.
When nothing remains above P, phase two falls back to global low. The pivot stays
fixed during an invocation, including across saved holes. Moving the midpoint
after every batch would create additional interior holes on large searches.

An indexed interval map supports endpoint and midpoint selection. Reservations
stop at the pivot, saved coverage and other reservations. At most three remain
active, and meeting fronts share the existing owner. The added pivot and active
reservations bound planner state by the original gap count plus four entries;
memory does not grow with the number of processed batches.

`plan()` reserves work without consuming coverage. `accept()` consumes the
successfully verified batch and advances the phase. Overflow re-plans from the
same endpoint with a smaller batch. Native output or durable completion handling
precedes acceptance. Each reservation retains its original accounting bounds and
records its own execution time, including replay and excluding other work/pauses.

Dance reuses existing forward/reverse minikey batch tags and GPU arithmetic.
Prepared targets survive direction and grant changes. Job/configuration/target
identities, schema 7, protocol, capabilities and `minikey-ordinal-v1` remain
unchanged. Existing `minikeys-v1` workers can recover forward. The default remains
forward; the previous three policies retain their selection rules.

Restart reconstructs missing intervals from receipts, recomputes the pivot and
starts low. It need not reproduce uninterrupted submission order. Exact coverage
and deduplicated verified results are the recovery contract. Geometry, device
and ordinal order may change. Scalar `--order`, BSGS `--tile-order` and block
claim policy remain separate. See [contract and executable example](C23_MINIKEYS_DANCE.md).

## Recorded validation

| Gate | Observed coverage |
| --- | --- |
| CPU release | 146/146 full-suite gates |
| Order oracle | 2,495 independent integer/candidate cases across four orders, both lengths, tiny/full domains, base-58/limb carries and UInt64 work bounds |
| Planner invariants | 144 order/geometry/layout scenarios, fragmented gaps, pivot inside a saved hole, singleton domains, changing work/batch sizes, smaller overflow plans, exact ownership and at most three active reservations |
| Primitive oracle | 1,781 cases each for portable/HIP/CUDA implementations |
| Native dance | 78 cases per backend: both lengths, all encodings, address/HASH160 targets, carries, endpoints, rejected candidates, overflow in all three phases, meeting tails and all eight device ordinals |
| Native regressions | 70 forward, 70 reverse and 78 both-ends cases per backend |
| Large candidate set | All admitted candidates in 1,048,576 ordinals are targets: 4,193 valid 22-character and 4,261 valid 30-character candidates in every order. Dance clips launches at its midpoint |
| Executor/fault gates | Existing direction-switch, retained-output/capacity/ownership checks and 29 allocation/runtime/corruption boundaries per direction and backend |
| Dance checkpoints | 20 cases per backend: six overflow/rejected/last-ordinal cases and fourteen killed restarts across both lengths; dance to/from forward, reverse, both-ends, and dance to dance |
| Three-front persistence | Acknowledge low, high and middle receipts before killing a dance owner; verify all three remain covered |
| Checkpoint regressions | Eight forward, twelve reverse and sixteen both-ends cases per backend |
| Host recovery | Sixteen lost-acknowledgment scenarios across both lengths, all four restart orders and fixed/adaptive work; overflow, three committed fronts, completed retry, false high receipt and wrong-mode rejection |
| Fragmented workers | Both lengths and fixed/adaptive work, three accepted islands, original six-capability owner compatibility, prior results and byte-identical lost-upload retry |
| Pause/visibility | Dance → reverse → dance for both lengths, plus all earlier policies; socket/signal pause, backup/restore quarantine and changed visible ordinals |
| Supervised workers | Four cases per backend for each of reverse, both-ends and dance: both lengths × HTTPS/disconnected-file, two grants, one prepared executor, exact reservation union and matching local/server results |
| Sanitizers | Eleven focused host ASan/UBSan gates |
| Public examples | CPU preparation and executable HIP/CUDA examples for reverse, both-ends and dance; dance covers 101 ordinals in six batches, with the public key in the second batch |

HIP passed all 33 selected gates. CUDA passed 32 initially; the forward-pause
startup check passed its focused rerun, giving passing evidence for all 33
distinct gates. The systems exposed eight
MI300X SPX/NPS1 and eight H200 devices. Device spots alternate key lengths;
this is not a full length/device Cartesian product or a fleet-scaling result.

The [manifest](baselines/C23_MINIKEYS_DANCE_VALIDATION.json) records source phases,
binary hashes, compiler/build settings, gate counts and artifact checksums.
[Algorithms](baselines/C23_MINIKEYS_DANCE_ALGORITHMS.json),
[recovery](baselines/C23_MINIKEYS_DANCE_RECOVERY.json),
[workers](baselines/C23_MINIKEYS_DANCE_WORKERS.json),
[examples](baselines/C23_MINIKEYS_DANCE_EXAMPLES.json) and
[raw logs](baselines/C23_MINIKEYS_DANCE_LOGS.tar.gz) retain observations.

## Findings and limits

The initial targeted sanitizer build used stale generated Make targets and could
not find the new `storage_minikey_dance_test`. Explicit CMake regeneration fixed
the build setup; the rebuilt eleven sanitizer gates passed. Both build logs are
retained. No production change was required during acceptance.

The initial CUDA forward-pause test timed out waiting for its runner to report
`running`. At the time, a separate OpenBLAS compilation was active on the H200
host. A resource snapshot recorded load 165.8 on 112 logical CPUs, 87 C compiler
processes and 83 blocked processes. Resource contention is a plausible cause;
the timeout alone does not establish a production defect. The unchanged test
passed its focused rerun. The initial suite log, resource snapshot and retry log
are retained rather than replacing the failed observation.

The independent sequence oracle tracks immutable reservations and uncovered
runs separately from the implementation's mutable interval map. The fragmented
host fixtures instead enumerate actual missing ordinals and remove each once.
Together they check midpoint fallback, clipped work, meeting fronts and phase
stability across overflow, beyond simply alternating whole work units.

GPU lanes execute concurrently; traversal defines submission and lane mapping,
not serial wall-clock execution. Admission counts are observations, not skip
rules. This change makes no throughput improvement claim. Sanitizer coverage is
limited to the eleven selected host gates, not device code or the full program.
Only public fixtures are archived; no enrollment credentials or journal databases.

## Reproduction

Use [BUILD.md](BUILD.md), coordinator/HTTPS support and the existing Apache test
fixture. Configure the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness. Keep test journals outside Git checkouts.

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure -R 'minikey|^work_unit$'
```

The focused sanitizer selection is `minikey_search_contract`,
`minikey_order_oracle`, `storage_minikey_checkpoint`, `storage_minikey_reverse`,
`storage_minikey_both_ends`, `storage_minikey_dance`, `coordinator_minikeys`,
`coordinator_minikey_reverse`, `coordinator_minikey_both_ends`,
`coordinator_minikey_dance` and `work_unit`. The full CPU suite provides broader
regression coverage. `tests/integration/minikey_dance_examples.py` runs the marked
[public example](C23_MINIKEYS_DANCE.md#executable-public-example) verbatim.
