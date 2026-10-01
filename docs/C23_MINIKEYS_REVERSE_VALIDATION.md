# C23 reverse minikey acceptance

Native minikey search, checkpoint runs and supervised HIP/CUDA workers support
`--ordinal-order forward|reverse` for 22- and 30-character minikeys. Reverse walks
actual candidate ordinals downward, preserving exact receipts and allowing
restarts in either direction. C23 remains partial: additional random traversal
semantics and other minikey orders still need separate contracts and parity gates.

## Decisions and implementation

Minikey ordinal coordinates already identify candidates independently of search
order. Keeping those coordinates avoids introducing an immutable candidate-index
mapping or changing job identity. Reverse work uses exclusive high cursors,
selects the highest missing interval first and maps logical lane i in [L,H) to
H-1-i. Concurrent GPU lanes do not imply serial wall-clock execution order.
Scalar `--order` and BSGS `--tile-order` remain separate options.

An internal `ReverseMinikeysV1` execution tag distinguishes the lane mapping and
participates in completion identity checks. Neither minikey algorithm accepts a
scalar-stride mapping. The GPU kernel adds count-1-i to the low batch endpoint,
using its existing checked limb addition; the CPU verifier independently derives
the expected ordinal, candidate, private scalar and encoding/HASH160 relation.
Direction comes from each submission, so a prepared executor can change direction
without retaining stale options. Target allocation survives worker grant changes.

Overflow discards the whole attempt and shrinks replay from the same high
endpoint. Only verified completion advances the cursor. Invalid check-byte
candidates still count as tested ordinals. Adaptive contiguous work units cannot
cross saved coverage; pause drains admitted work and restart uses the exact
complement. Stored results remain `(ordinal, canonical target)` relations.

Job/configuration/target identities, schema 7, protocol and capabilities remain
unchanged. The original `minikeys-v1` capability is sufficient. Older minikey
workers can recover these jobs forward. `checkpoint create` and non-minikey
runners reject `--ordinal-order`, including explicit forward. See
[the contract and public example](C23_MINIKEYS_REVERSE.md).

## Recorded validation

The available stacks exposed eight MI300X SPX/NPS1 devices and eight H200 devices.
Device spot checks alternate minikey lengths; the full length/device Cartesian
product, fleet scaling and new partition configurations are outside this claim.

| Gate | Observed coverage |
| --- | --- |
| CPU release | 134/134 full-suite gates passed; the isolated recovery fixture also passed afterward |
| Order oracle | 1,251 independent integer/candidate sequence cases covering both directions, both domains, tiny ranges, changing work/batch boundaries, base-58/limb carries, UInt64-sized work, exact endpoints and invalid inputs |
| Primitive oracle | 1,781 cases per portable/HIP/CUDA implementation, using Python integers/hashlib and public examples |
| Native search | 70 reverse and 70 forward cases per backend: both lengths, all encoding choices, address/HASH160 identity, tails, first/last ordinals, carry boundaries, rejected candidates, overflow and all eight device ordinals |
| Maximum batch | Every admitted candidate in 1,048,576 ordinals is a target: 4,193 valid 22-character and 4,261 valid 30-character candidates, checked in both directions on both backends |
| Executor ownership | Existing ownership, capacity, retained-output and overflow gates plus direction changes on one prepared executor |
| Fault injection | 29 allocation/runtime/output-corruption boundaries per direction and backend; failed owners cannot emit coverage and must be recreated |
| Reverse checkpoints | 12 cases per backend: six overflow/rejected/last-ordinal cases and six killed-process transitions across both lengths, with changed geometry, exact complements and deduplication |
| Forward checkpoint regression | Eight cases per backend, retaining identity/length/encoding validation and killed recovery |
| Host recovery | Reverse acknowledgment loss followed by both restart directions and fixed/adaptive work for both lengths; wrong-direction completion identity, false receipts, pause and completed retry |
| Fragmented transfer | Three accepted islands, both lengths/directions and fixed/adaptive work, original six-capability worker compatibility, prior-owner results and byte-identical lost-upload retry |
| Pause/visibility | Forward regression plus reverse → forward → reverse for both lengths, socket/signal pause, durable results, backup/restore quarantine and changed visible ordinals |
| Workers | Four reverse cases per backend: both lengths × HTTPS/disconnected-file transport; two grants per case, one prepared executor, descending work-unit union and identical local/server results |
| Sanitizers | Seven focused ASan/UBSan gates passed across the initial run and the corrected recovery-fixture rerun |
| Public example | CPU preparation and full HIP/CUDA reverse search/checkpoint, followed by a completed-grant forward retry and integrity check |

Each GPU acceptance invocation passed all 19 selected gates. The final fixture
and corrected direct-kernel pause metadata were additionally checked with three
focused gates on each backend. The
[manifest](baselines/C23_MINIKEYS_REVERSE_VALIDATION.json) records source phases,
binary hashes, build/compiler settings and artifact checksums.
[Algorithms](baselines/C23_MINIKEYS_REVERSE_ALGORITHMS.json),
[recovery](baselines/C23_MINIKEYS_REVERSE_RECOVERY.json),
[workers](baselines/C23_MINIKEYS_REVERSE_WORKERS.json),
[examples](baselines/C23_MINIKEYS_REVERSE_EXAMPLES.json) and
[raw logs](baselines/C23_MINIKEYS_REVERSE_LOGS.tar.gz) retain the observations.
No enrollment credentials or journal databases are archived.

## Findings and limits

The first recovery fixture reused a project across scenarios, so job creation
correctly returned an already completed job and no acknowledgment failure could
be triggered. Giving scenarios separate projects fixed that test assumption.
The expanded fixture then timed out under sanitizers because a shared journal
repeatedly audited all previous scenarios' jobs and results. Isolating journals
retained all direction/adaptation cases and passed in about 100 seconds under
ASan/UBSan, within its revised 180-second watchdog. Existing forward tests retain
the corruption/backup checks. No production fix was needed for these fixture
issues. Initial failures and final passing evidence are retained.

The generic pause harness previously labeled its default kernel as stepped even
for minikeys, whose only kernel is direct. Its minikey report now records direct
for all stages; both pause policies were rerun on both backends.

This slice adds exact traversal, not a throughput optimization claim. Admission
counts are observed test results, never statistical skip rules. Sanitizer
coverage is limited to the seven listed host gates; it does not cover device
code or the whole application. Reverse changes execution order while preserving
coverage, rather than reproducing legacy random enumeration.

## Reproduction

Use [BUILD.md](BUILD.md), coordinator/HTTPS support and the existing Apache test
fixture. Set the [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits)
for the shared pause harness and use a journal parent outside Git checkouts.

```sh
cmake -S . -B BUILD
cmake --build BUILD -j 12
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure -R 'minikey|^work_unit$'
```

The full CPU suite supplies broader scheduler and scalar-family regression
coverage. The focused sanitizer selection is `minikey_search_contract`,
`minikey_order_oracle`, `storage_minikey_checkpoint`, `storage_minikey_reverse`,
`coordinator_minikeys`, `coordinator_minikey_reverse` and `work_unit`.
`tests/integration/minikey_reverse_examples.py` executes the marked commands
verbatim from [the public example](C23_MINIKEYS_REVERSE.md#executable-public-example).
