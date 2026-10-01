# C23 both-ends minikey acceptance

Native minikey search, checkpoint runs and supervised HIP/CUDA workers support
`--ordinal-order both-ends` for 22- and 30-character minikeys. The policy starts
low and alternates successful batches between the lowest and highest missing
ordinals. Exact canonical receipts permit restart with any supported order.
C23 remains partial: additional random semantics and other minikey orders still
require separate contracts and parity gates.

## Decisions and implementation

A shared `MinikeyBatchPlanner` selects batches for native searches and durable
owners. Its `plan()` reserves contiguous work without consuming coverage.
`accept()` consumes the successfully verified batch and advances the phase.
Overflow can re-plan with a smaller bound from the same endpoint. The native
owner writes output before acceptance; the durable owner validates and records
completion before advancing its local planner.

Missing intervals remain sorted in canonical ordinal coordinates. At most two
work reservations can be active. Each new reservation stops at saved coverage or
another reservation. When the two fronts meet, both use the existing owner and
clip batches to its unconsumed middle. Adaptive timing belongs to the reservation
and includes its replay work, excluding pauses and execution of the opposite
reservation. Planner storage scales with the existing missing-interval set plus
at most two active reservations, not with the ordinal domain's width.

Each batch uses the existing forward or reverse minikey execution tag. A high
batch maps logical lane i to H-1-i; a low batch maps it to L+i. Prepared GPU
executors already read this mapping per submission and retain target allocation
across grants. No new GPU kernel, serialized job configuration, schema,
capability or protocol version is needed. The C++ execution option is now
`CheckpointOptions::minikey_order`, using an enum instead of the previous boolean.

The checkpoint runner shares completion validation, counters and commit handling
with scalar searches. Minikey selection uses the new planner; scalar selection
retains its existing mapping. A completion with correct bounds but the wrong
lane tag is rejected before it can add coverage. `checkpoint create` and
non-minikey runners reject an explicit ordinal-order option.

Restart rebuilds the exact missing complement and starts low for both-ends.
Alternating phase is process-local and is not persisted. Existing `minikeys-v1`
workers can recover the same job forward. See [the contract and public
example](C23_MINIKEYS_BOTH_ENDS.md).

## Recorded validation

| Gate | Observed coverage |
| --- | --- |
| CPU release | 140/140 full-suite gates, including the shared scalar checkpoint regression coverage |
| Order oracle | 1,873 independent integer/candidate sequence cases across three orders, both domains, tiny/huge ranges, base-58/limb carries, UInt64 work bounds, changing work/batch boundaries and exact endpoints |
| Planner invariants | 36 fragmented-domain geometry/order scenarios, changing work and batch bounds, repeated smaller plans without acceptance, disjoint ownership, at most two active reservations and exact reservation completion |
| Primitive oracle | 1,781 cases per portable/HIP/CUDA implementation using Python integers/hashlib and public examples |
| Native both-ends | 78 cases per backend: both lengths, all encodings, address/HASH160 identity, endpoint tails, carry boundaries, rejected candidates, forced overflow, extra meeting-point cases and all eight device ordinals |
| Native regressions | 70 forward and 70 reverse cases per backend |
| Maximum batch | All admitted candidates in 1,048,576 ordinals are targets: 4,193 valid 22-character and 4,261 valid 30-character candidates, tested in all three orders; the single-batch both-ends case starts low |
| Executor/fault gates | Existing retained-output/capacity/ownership checks, direction changes on one prepared executor, and 29 allocation/runtime/corruption boundaries per direction and backend |
| Both-ends checkpoints | 16 cases per backend: six overflow/rejected/last-ordinal cases and ten killed restarts across both lengths, covering both-ends → forward/reverse/both-ends and forward/reverse → both-ends |
| Endpoint persistence | Before killing a both-ends owner, acknowledge both a low and a high batch and verify both endpoints remain covered after restart |
| Checkpoint regressions | Eight forward and twelve reverse cases per backend |
| Host recovery | Twelve acknowledgment-loss cases across both lengths, all three restart orders and fixed/adaptive work; forced overflow, two committed endpoints, completed retry, wrong high-batch identity and wrong-mode rejection |
| Fragmented workers | Both lengths and fixed/adaptive work, three accepted islands, original six-capability worker compatibility, prior-owner results and byte-identical lost-upload retry; forward/reverse regressions retained |
| Pause/visibility | Both-ends → reverse → both-ends for each length, plus forward and reverse regressions; socket/signal pause, retained results, backup/restore quarantine and changed visible ordinals |
| Supervised workers | Four both-ends and four reverse cases per backend: both lengths × HTTPS/disconnected-file transport, two grants per case, one prepared executor, exact work-unit union and matching local/server results |
| Sanitizers | Nine focused ASan/UBSan gates passed |
| Public examples | CPU preparation plus executable HIP/CUDA examples for both-ends and reverse; the both-ends example covers 101 ordinals in six batches and finds the public candidate in its second batch |

Each GPU invocation passed all 26 selected gates. The stacks exposed eight
MI300X SPX/NPS1 and eight H200 devices. Device spot checks alternate lengths;
this is not a full length/device Cartesian-product or fleet-scaling claim.
The [manifest](baselines/C23_MINIKEYS_BOTH_ENDS_VALIDATION.json) binds source phases,
binary hashes, build/compiler settings and artifact checksums.
[Algorithms](baselines/C23_MINIKEYS_BOTH_ENDS_ALGORITHMS.json),
[recovery](baselines/C23_MINIKEYS_BOTH_ENDS_RECOVERY.json),
[workers](baselines/C23_MINIKEYS_BOTH_ENDS_WORKERS.json),
[examples](baselines/C23_MINIKEYS_BOTH_ENDS_EXAMPLES.json) and
[raw logs](baselines/C23_MINIKEYS_BOTH_ENDS_LOGS.tar.gz) retain observations.

## Findings and limits

The first expanded native CLI harness reused the selected order variable while
checking invalid option values. This caused three CPU harness failures before
GPU validation. Renaming that loop variable fixed the test setup; the three
focused reruns and subsequent full CPU/GPU suites passed. No production fix was
needed. Initial failures and the passing rerun are retained in the evidence.

The independent planner model tracks immutable reservations and global uncovered
endpoints, whereas the implementation updates a sorted interval map. Separate
fragmented-domain tests enumerate actual missing ordinals and remove each exactly
once, checking both ownership and global endpoint selection. These checks cover
meeting fronts that cannot be tested by alternating whole work units alone.

GPU lanes execute concurrently; the policy defines submission/lane mapping,
not serial wall-clock execution. Admission counts are observed results, never
statistical skip rules. No throughput improvement is claimed. Sanitizers cover
the nine selected host gates, not device code or the entire application.
No enrollment credentials or journal databases are archived.

## Reproduction

Use [BUILD.md](BUILD.md), coordinator/HTTPS support and the existing Apache test
fixture. Configure the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness, and use a journal parent outside Git checkouts.

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure -R 'minikey|^work_unit$'
```

The focused sanitizer selection is `minikey_search_contract`,
`minikey_order_oracle`, `storage_minikey_checkpoint`, `storage_minikey_reverse`,
`storage_minikey_both_ends`, `coordinator_minikeys`, `coordinator_minikey_reverse`,
`coordinator_minikey_both_ends` and `work_unit`. The full CPU suite supplies broader
regression coverage. `tests/integration/minikey_both_ends_examples.py` executes the
marked [public example](C23_MINIKEYS_BOTH_ENDS.md#executable-public-example) verbatim.
