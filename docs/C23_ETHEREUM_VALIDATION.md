# C23 Ethereum family acceptance

The Ethereum address family is complete on MI300X HIP and H200 CUDA. Native
`ethereum` searches, durable checkpoints and authenticated/offline worker queues
use exact scalar intervals with independent CPU verification. C23 remains
partial. Vanity was pending at this acceptance; its later
[separate acceptance](C23_VANITY_VALIDATION.md) completes that family. Minikeys
and other families still need their own parity gates.
Bitcoin P2PKH/HASH160 retains its [separate acceptance](C23_VALIDATION.md).

## Behavior and decisions

The [contract](C23_ETHEREUM.md) specifies Keccak-256 of the 64-byte affine
`X || Y`, followed by the last 20 digest bytes. The SEC1 prefix is excluded.
The portable primitive uses Keccak's suffix `0x01`, and independent tests
explicitly distinguish every valid vector from SHA3-256. CPU verification uses
the retained Keccak implementation; the test oracle uses pinned PyCryptodome
3.23.0 and pinned libsecp256k1 public keys.

Files accept 40 hex digits with an optional lowercase `0x` prefix. Uniform case
is accepted; mixed case must pass ERC-55. Binary targets are sorted/deduplicated
and bound to an Ethereum-specific digest. Work algorithm 3 and journal mode 4
prevent cross-family execution or persistence. Each scalar emits at most one
address, but target count never bounds scalar preimages. Overflow attempts credit
nothing and replay smaller intervals, down to one scalar.

The existing scalar checkpoint runner supplies ownership, fencing, pause,
adaptive work planning and exact complement replay. No journal migration or
schema change was needed. Worker queues retain immutable targets and GPU
allocations across grants. Coordinators require `ethereum-v1` before allocation,
renewal, updates or cached receipt replay; the two previously shipped capability
sets remain accepted for their existing families. Deploy the new coordinator
before the new worker.

GPU kernel/executor translation units remain separate from Bitcoin HASH160 to
preserve its accepted serialization and register paths. Shared point arithmetic
and checkpoint ownership remain common. This is a correctness delivery; no new
throughput or tuning result is claimed.

## Validation evidence

The [acceptance manifest](baselines/C23_ETHEREUM_VALIDATION.json) records source
phases, binary/source hashes, device inventories, the independent oracle wheel
hash and all artifact checksums. Evidence is phased: earlier algorithm binaries
are identified by their embedded hashes, separately from the final worker build.

| Gate | Result |
| --- | --- |
| CPU release | 33 relevant contract, scheduler, checkpoint and coordinator tests pass |
| ASan/UBSan | Four Ethereum host contract, hash, storage and coordinator tests pass |
| Keccak oracle | 412 portable and native cases per backend, all one-block lengths and rejection boundaries |
| Native search | 48 independent cases and 25 rejections per kernel per backend; all eight visible ordinals |
| Executor ownership/faults | Direct/stepped receipts, stale/foreign tickets, dense tails and 31 injected failure boundaries per backend |
| Exact ranges | High scalars, multi-limb carries, curve-order tails and maximum 1,048,576-scalar batches with all cached offset bits |
| Recovery | Kill after durable acknowledgment, changed batch geometry, remaining-interval replay, completed-grant retry and false-receipt rejection |
| Pause/restart | Repeated pause/resume, graceful signals, backup quarantine and changed device visibility |
| Worker transport | Persistent two-grant owner, HTTPS sync, disconnected file exchange, later acknowledgment and empty outbox |
| Regression | Existing Bitcoin/xpoint executor and fault gates, Bitcoin pause, shared checkpoint modes and CUDA context isolation |
| Documentation | CPU, HIP and CUDA execute the marked quickstart commands, including all four durable families |

[Algorithm reports](baselines/C23_ETHEREUM_ALGORITHMS.json),
[recovery reports](baselines/C23_ETHEREUM_RECOVERY.json),
[offline reports](baselines/C23_ETHEREUM_OFFLINE.json),
[executable examples](baselines/C23_ETHEREUM_EXAMPLES.json) and the
[raw logs](baselines/C23_ETHEREUM_LOGS.tar.gz) retain the detailed observations.
Only public scalar fixtures were used. Credential contents and journal databases
are excluded from the archive.

## Reproduction and limits

Install the independent test dependency with
`python3 -m pip install -r tests/oracle/requirements.txt`, or set
`KEYHUNT_KECCAK_ORACLE_ROOT` to an extracted, verified PyCryptodome 3.23.0 wheel.
The validation hosts used the latter because the local system Python lacked pip;
production has no new Python dependency. Build using [BUILD.md](BUILD.md), then
run `ctest --test-dir BUILD -R 'ethereum|keccak' --output-on-failure` and the
shared checkpoint/HTTPS/offline suites recorded in the manifest. The executable
[quickstart](GPU_QUICKSTART.md) covers commands, job creation and durable replay.

Hardware validation uses eight visible SPX/NPS1 MI300X devices and eight
MIG-disabled H200 devices. Sequential ordinal coverage does not establish
Ethereum multi-device scaling. No new partition/MIG, calibrated block width,
throughput or public coordinator deployment claim is made. Ethereum contract
addresses, ENS and chain-specific checksum conventions are outside this scalar
EOA derivation contract. Legacy `-m` flags retain their CPU behavior.
