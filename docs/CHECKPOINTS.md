# Verified local checkpoints (C13)

C13 connects exact xpoint/BSGS execution to the C12 [local journal](STORAGE.md).
The guarantee is at-least-once computation with exact locally acknowledged
coverage and deduplicated CPU-verified matches. It is standalone storage on one
host; no remote synchronization, server acknowledgment or distributed outbox is
claimed. Those belong to C15. Graceful signal handling and pause controls remain
C14. Existing `keyhunt xpoint` and `keyhunt bsgs` commands retain their explicitly
volatile C09/C11 behavior.

## Binding real inputs

A checkpoint job uses the same immutable C12 manifest/root/block grid, with target
and algorithm digests derived from actual loaded inputs. Canonical target sets
are persisted as sorted unique 32-byte X values or 65-byte uncompressed SEC1
points. The existing target loaders reject malformed/noncanonical inputs;
compressed/uncompressed encodings of the same BSGS point identify one target.

The algorithm fingerprint is SHA256 of exactly 50 bytes: `khsearch` (8 bytes),
semantic version 1, mode byte (1=xpoint, 2=BSGS), big-endian 64-bit `m`, and the
32-byte table checksum. Xpoint uses zero `m` and checksum. Semantics are secp256k1,
stride one, exhaustive all-target coverage. BSGS binds the validated C10 cache
including its table size and checksum. Changing `m` or rebuilding a differently
encoded cache requires a new job; incompatible progress is never silently reused.

Devices, partition mode, kernel variant, launch/target batch sizes and checkpoint
cadence are excluded from this fingerprint. They may change on restart while
preserving the exact accepted scalar union. A `keyhunt-C13-v1` software-format
tag is recorded with the binding; it is not a source-commit attestation.

A C12 job with synthetic/raw coverage cannot be promoted into a verified search.
The bind operation rejects existing partial or finished coverage unless a search
binding was already registered. For bound jobs, raw `record_coverage` is disabled.
Only the checkpoint owner can call the private result/coverage transaction.

## Schema version 2 and migration

`schema_v1.sql` remains byte-for-byte unchanged. Version 2 adds canonical search
bindings, local executor generations, deduplicated results and retained checkpoint
payloads. All carry project/job composite keys; result pagination uses a bounded
signed-64-bit row ID, while scalar/block values remain fixed-width 256-bit BLOBs.

An existing v1 journal is validated and backed up to a private `pre-v2-UUID` child
directory before its schema changes. The writer reservation excludes concurrent
mutations while a separate read connection creates the sealed snapshot. Migration
and the new schema/version digest commit together. Fresh journals create both
migrations in one transaction. Unsupported versions or changed migration hashes
fail. Pre-migration snapshots retain v1 and are quarantined; opening one with a
new binary may itself migrate it, so use a read-only SQLite connection to inspect
its original schema version without modification.

The v2 tables reference existing v1 data; migration does not grant synthetic
coverage verified status or reactivate a quarantined snapshot. C12's exclusive
backup publication and epoch/quarantine rules remain in force.

## Execution and commit order

The checkpoint owner takes an exclusive local guard, audits the journal, validates
the resolved targets/configuration, verifies the grant, and durably allocates a
fresh executor generation before any GPU submission. Assignment owner, epoch,
generation and current expiry are rechecked before each batch and inside each
checkpoint transaction. A replacement executor receives a fresh generation even
when resuming the same assignment.

One stable `executor.lock` file per journal uses a nonblocking advisory lock.
It is private, never unlinked during handoff, and released automatically on
process exit. This deliberately permits one checkpoint owner per local journal
in C13; multiple independent blocks/devices in one journal await the C20 supervisor.
C12 allocation and inspection may still use concurrent connections. This is a
trusted local ownership guard, not remote authentication or protection against an
operator bypassing the application protocol.

Executor construction/upload is deferred until preflight succeeds. On exit or an
exception, draining and destroying the executor precedes releasing the guard.
A caller's assertion that an old executor stopped is still required for a live
assignment transfer; fencing alone does not stop a GPU.

For each bounded successful completion:

1. Validate that the receipt matches the submitted interval, input digests and
   expected work counts. Overflow attempts contribute no matches or coverage.
2. CPU-verify each accepted scalar against its exact canonical target, reject
   duplicates/out-of-batch candidates, then collect the completed interval.
3. Persist found matches immediately. Coalesce no-match coverage in memory until
   the checkpoint interval (default ten seconds), a bounded interval-buffer limit,
   or normal block completion.
4. In one FULL WAL transaction, insert/deduplicate results, merge accepted coverage,
   coalesce finished blocks, and store the idempotency receipt plus its canonical
   payload. Only after COMMIT may output acknowledge the checkpoint.

Checkpoint timing is checked between bounded completions, not in a signal handler
or background thread. It is a cadence, not a hard ten-second wall-clock deadline;
kernel, verification and storage latency can extend it. Zero selects every
completed interval. Pending coverage has at most 1,024 disjoint intervals;
adjacent batches coalesce. There is one executor result slot and no unbounded
queue of GPU batches. Match buffers retain the existing executor capacity limits.

The owner defensively CPU-verifies returned matches again at the persistence
boundary because executor result structs are mutable host data. This adds work
only for candidates, not every scalar in a no-match GPU range.

## BSGS target groups and replay

A BSGS interval is accepted once, after every canonical target group completes
without overflow. Matches from earlier groups are persisted immediately even
though that tile has no accepted scalar coverage yet. A crash during the tile
therefore replays all its groups; unique `(project,job,scalar,target)` result keys
deduplicate repeated matches. Candidate overflow reduces the group size and
replays the same target cursor. Xpoint overflow similarly reduces the scalar
batch size without crediting its retained candidate prefix.

On restart, planning starts from the complement of committed intervals, not the
previous process's counters or stdout. Partial BSGS group cursors are intentionally
not persisted: replaying a bounded tile keeps restart independent of launch/group
geometry. A retry after the final commit performs no new GPU work. Expired or
transferred active assignments cannot resume until explicitly recovered with a
valid fence. Corrupt target files/table caches or mismatched job inputs fail
before GPU work.

A lost output acknowledgment never undoes COMMIT. Query durable results and resume
with the retained grant. Exceptions do not implicitly flush pending coverage;
work since the last accepted checkpoint may replay. SIGKILL/driver failure and
normal completion have the same accepted-state boundary.

## Integrity and retained history

Every checkpoint's canonical payload includes its block, assignment/executor
generations, epoch, accepted intervals and verified scalar/target pairs. Its SHA256
is stored in the existing idempotency receipt. `state check` and execution preflight
reconstruct the interval union and match set from those receipts, compare them with
the authoritative tables, and re-verify stored matches against canonical inputs.
They also perform the C12 SQLite/foreign-key, manifest, sparse-tree and containment
checks. Missing results, edited coverage, missing checkpoint payloads, corrupted
payload hashes and changed canonical bindings are rejected.

This detects inconsistent/corrupted state, not deliberate rewriting of every
payload, checksum and table by a trusted filesystem owner. Checksums are not
authentication. Full audits scale with retained history and matches; they run at
preflight/explicit inspection, outside the GPU kernel loop.

Checkpoint payloads and idempotency/audit history are retained through compaction.
This preserves replay/audit evidence but grows with actual checkpoints; C15 must
define any archival/retention protocol. There is no second filesystem progress
journal. Results and intervals live in the same SQLite commit.

## Initial verification

The CPU mock executors compute real curve points for small known ranges through
the same checkpoint owner. Tests cover seeded boundary matches, no-match batching,
BSGS partial-group interruption/deduplication, changed batch geometry, overflow,
expired/transferred ownership, cleanup-before-unlock, invalid result/count
rejection, v1 migration with a sealed backup, and corrupted state.

The dedicated fault binary terminates real processes at seven boundaries for each
mode: after execution, after CPU verification, after result insertion, after
coverage changes, before COMMIT, after COMMIT and after acknowledgment. All fourteen
cases recover the expected result set and exact coverage. The hooks are compiled
only into test binaries; production has no environment-controlled fault injection.
These are process-failure tests, not certification of power-loss behavior in the
underlying storage hardware.

## Public commands

`checkpoint create` resolves actual inputs on the CPU and registers the canonical
binding. Use `state claim` to reserve a block, then `checkpoint run` to execute
that grant on one HIP logical device. All actions accept `--state-dir` or the
C12 state-directory environment defaults.

~~~sh
export KEYHUNT_STATE_DIR="$HOME/.local/state/keyhunt-durable"
./build/cpu-release/keyhunt state project-create --name "Checkpoint example"
# Set PROJECT to the returned project UUID.
printf '%s\n' 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 > /tmp/keyhunt-x.txt
./build/cpu-release/keyhunt checkpoint create --project "$PROJECT" \
  --mode xpoint --range 1:101 --block-width 100 --targets /tmp/keyhunt-x.txt
# Set JOB to the returned job digest.
./build/cpu-release/keyhunt state claim --project "$PROJECT" --job "$JOB" \
  --owner workstation --request allocation-001
# Set GRANT to the returned assignment's complete grant token.
./build/hip-release/keyhunt checkpoint run --backend hip --grant "$GRANT" \
  --targets /tmp/keyhunt-x.txt --device 0 --batch-size 256
./build/cpu-release/keyhunt checkpoint results --project "$PROJECT" --job "$JOB"
~~~

The range is half-open and scalar endpoints/block widths are hexadecimal. Other
numeric options are decimal. Repeat the same `checkpoint run` with the retained
grant after a process failure; no new claim is needed while ownership is valid.
Device/batch/kernel choices may change. The same input file path is not required,
but its canonical target content must match.

For BSGS, prepare a C10 table, use `--mode bsgs --table FILE` when creating the job,
and provide the same compatible table on execution:

~~~sh
./build/cpu-release/keyhunt bsgs-table build --m 257 --output /tmp/checkpoint-babies.khb
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 > /tmp/keyhunt-pub.txt
./build/cpu-release/keyhunt checkpoint create --project "$PROJECT" \
  --mode bsgs --range 1:10001 --block-width 10000 \
  --targets /tmp/keyhunt-pub.txt --table /tmp/checkpoint-babies.khb
# Claim the returned job as above, then use that new GRANT:
./build/hip-release/keyhunt checkpoint run --backend hip --grant "$GRANT" \
  --targets /tmp/keyhunt-pub.txt --table /tmp/checkpoint-babies.khb \
  --giant-batch 256 --target-batch 64 --group-size auto
~~~

| Action | Options |
| --- | --- |
| `create` | Required `--project`, `--mode xpoint\|bsgs`, `--range`, `--block-width`, `--targets`; BSGS requires `--table` and accepts `--host-memory` |
| `run`, common | Required `--backend hip`, `--grant`, `--targets`; optional `--device` (0), `--candidate-capacity` (1024), `--checkpoint-seconds 0..60` (10) |
| `run`, xpoint | `--batch-size 1..1048576` (1048576), `--kernel stepped\|direct` (stepped); capacity 1..1048576 |
| `run`, BSGS | Required `--table`; `--giant-batch` (16384), `--target-batch 1..64` (64), product at most 1048576; capacity 1..65536; `--group-size auto\|1\|8`; `--host-memory` (1073741824 bytes), `--reserve-bytes` (67108864 bytes) |
| `results` | Required `--project`, `--job`; optional `--after` (0), `--limit 1..1000` (100) |

The BSGS memory limit retains the C11 target/table/executor budgeting contract.
Canonical journal bindings, audit history and SQLite caches add host memory outside
that executor budget. Full preflight audits scale with historical state; the GPU
loop remains bounded by launch/candidate options.

Creation returns one JSON object with project/job/target/algorithm digests.
Execution emits NDJSON checkpoint acknowledgments and a final summary. A checkpoint
record lists exactly the newly accepted intervals and reports local durability.
A BSGS subgroup may emit `durable_results: true` with `durable_coverage: false` and
an empty interval list. A final complete summary confirms the assigned block,
not every block in the job. `resumed_scalars` counts previously accepted coverage;
`computed_scalars` counts newly accepted coverage this invocation. Device steps
and match observations include replayed work, so they are not distinct-result
counts. The results query is the authoritative deduplicated match set.

Results are ordered by their durable row ID. Pass `next_after` to the next query
until `results` is empty; IDs are decimal strings. Each row includes its wide
scalar/block, canonical target index and target bytes. IDs can have gaps.
These local commands do not implement remote authentication.

CPU-only builds support creation, claims, checks and result inspection. A HIP
execution request on such a build fails explicitly. Signals retain ordinary
process-exit behavior in C13; only already committed progress survives. Graceful
drain/checkpoint controls are C14 work.

## End-to-end verification

The command regression derives known targets from the pinned libsecp256k1 oracle.
It tests high-bit xpoint ranges, BSGS near the curve-order endpoint, canonical
duplicate/order equivalence, wrong targets/tables, corrupted table files, no-match
batching, result pagination, and overflow replay. Both real HIP modes are killed
after a checkpoint acknowledgment, then restarted with different launch geometry.
The journal retains acknowledged matches and resumes the accepted complement.
A concurrent second owner is rejected; a `/dev/full` output failure also preserves
the committed state. These cases use synthetic public fixtures on logical device 0.

~~~sh
ctest --preset cpu-release -L storage
ctest --preset hip-release -R checkpoint
~~~
