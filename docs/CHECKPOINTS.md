# Verified local checkpoints (C13)

The checkpoint commands connect exact HIP/CUDA xpoint, BSGS, HASH160, Ethereum, vanity and minikey execution to the
[local journal](STORAGE.md). The guarantee is at-least-once computation with exact
locally acknowledged coverage and deduplicated CPU-verified matches. Start with
the runnable [GPU quickstart](GPU_QUICKSTART.md). C13 introduced this contract;
C18 validated it on CUDA. Historical acceptance results below retain their dates
and scope; the current schema is [version 7](STORAGE.md#schema-and-migration).

Standalone execution has no server acknowledgment. The optional
[C15 coordinator](COORDINATOR.md) adds HTTPS synchronization and a durable worker
outbox; [C20](MULTI_GPU.md) adds concurrent owners on both backends. C14's
[graceful signals and controls](PAUSE_RESUME.md) apply to durable execution.
Ordinary `keyhunt xpoint`, `bsgs`, `hash160`, `address`, `ethereum`, `vanity` and `minikeys` commands remain volatile.

## Binding real inputs

A checkpoint job uses the same immutable C12 manifest/root/block grid, with target
and algorithm digests derived from actual loaded inputs. Canonical target sets
are persisted as sorted unique 32-byte X values or 65-byte uncompressed SEC1
points, plus C23's 21-byte encoding/HASH160 relations, 20-byte Ethereum addresses,
36-byte vanity prefix relations and 22-byte minikey length/encoding/HASH160 relations. The existing target loaders reject
malformed/noncanonical inputs;
compressed/uncompressed encodings of the same BSGS point identify one target.

For version 1, the algorithm fingerprint is SHA256 of exactly 50 bytes: `khsearch` (8 bytes),
semantic version 1 for unit stride, mode byte (1=xpoint, 2=BSGS, 3=HASH160, 4=Ethereum, 5=vanity, 6=minikeys), big-endian 64-bit `m`, and the
32-byte table checksum. Xpoint, HASH160, Ethereum, vanity and minikeys use zero `m` and checksum. Semantics are secp256k1,
stride one for version 1, exhaustive all-target coverage. Nonunit scalar strides
use version 2 with an additional 96 bytes for the original range and stride, as
described below. Reverse traversal uses version 3 with the same 146-byte layout
and permits stride one ([reverse contracts](C23_REVERSE.md)). BSGS binds the validated C10 cache
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

## Minikey ordinal jobs

Use `checkpoint create --mode minikeys --length 22|30 --targets FILE`, with
`--input-format address|hash160` and `--encoding compressed|uncompressed|both`.
Defaults are address and both. Repeat length and input/encoding options when
running the grant. Length and encodings are immutable target identity. Only
`--kernel direct` is accepted; it is also the minikey default.

Ranges and block widths are hexadecimal **candidate ordinals**, as defined in
the [contract](C23_MINIKEYS.md). `minikeys inspect --key TEXT` supplies a starting
ordinal. The journal retains ordinal coordinates, including checksum-rejected
candidates, so restart computes the exact uncommitted candidate complement.
Internal receipt/SQL fields named `scalar` contain ordinals in mode 6; public
result views expose `ordinal`, `minikey` and the derived private `scalar`.
Summaries use `resumed_ordinals` and `computed_ordinals` and label their
`coordinate_space` as `minikey-ordinal-v1`. Each `(ordinal,target)` relation is
preserved. Overflow commits no prefix. See the [executable example](GPU_QUICKSTART.md).

## Bitcoin vanity prefix jobs

Use `checkpoint create --mode vanity --targets FILE` with case-sensitive Bitcoin
mainnet P2PKH prefixes. Each line starts with `1` and contains 1..34 Base58
characters. Repeat `--encoding compressed|uncompressed|both` (default both) on
create and run. Canonical targets bind mode 5 and preserve prefix length, case,
encoding and zero padding. Results carry those full 36 bytes in `target_bytes`.

Select scalar `--batch-size` and `--kernel stepped|direct|glv`. Capacity must fit the
sum of distinct prefix lengths for each enabled encoding (at most 68). Overflow
retries the whole attempted interval before committing coverage. Every overlapping
prefix match at the same scalar remains a separate relation; exact duplicate
relations deduplicate. The CPU validates the complete P2PKH address before each
commit. Input-format and BSGS options are rejected. See [contracts](C23_VANITY.md)
and the [executable example](GPU_QUICKSTART.md).

## Ethereum jobs

Use `checkpoint create --mode ethereum --targets FILE` with 40 hexadecimal
address digits per line, optionally prefixed with `0x`. Uniform case is accepted;
mixed case must pass ERC-55. `checkpoint run` uses the same target file and scalar
`--batch-size`/`--kernel` options. Encoding/input-format and BSGS table options are
rejected. Canonical binary addresses are sorted/deduplicated and bind mode 4;
capitalization or prefix changes cannot create a different job.

Every candidate is checked with independent CPU secp256k1/Keccak before a commit.
There is one address per scalar; overflow attempts retain no coverage. Results
carry the full 20-byte address in `target_bytes`. The existing ownership, fencing,
pause, backup quarantine and remaining-interval replay rules apply. See
[the contract](C23_ETHEREUM.md) and [executable example](GPU_QUICKSTART.md).

## Bitcoin address and HASH160 jobs

Use `checkpoint create --mode hash160 --targets FILE` for raw 40-digit hashes,
or `--mode address` for Bitcoin mainnet P2PKH Base58Check. Both store mode 3.
`--encoding compressed|uncompressed|both` defaults to both and is part of the
canonical target set. Equivalent address/raw files produce the same job identity.

On `checkpoint run`, use `--input-format hash160` (default) or `--input-format address`
with the matching file, and repeat the original `--encoding` selection. Changed
encoding or target bytes fail binding validation. Select `--kernel stepped|direct|glv`
and `--batch-size` as for xpoint. With both encodings, candidate capacity must be
at least two; dense overflow retries smaller scalar intervals before coverage
advances. Results include `target_bytes`: `01` or `02` followed by the full hash.
The scalar/target pair is unique; two encoding results at one scalar remain distinct.

C23 uses the existing schema and receipt encoding. It does not change published
migrations, weaken restored-state quarantine or bypass lease/executor fences.

## Schema version 2 and migration

This section records the original C13 v1-to-v2 migration. Current upgrades
retain one sealed backup before applying all missing migrations through v7; see
[the current schema contract](STORAGE.md#schema-and-migration).

`schema_v1.sql` remains byte-for-byte unchanged. Version 2 adds canonical search
bindings, local executor generations, deduplicated results and retained checkpoint
payloads. All carry project/job composite keys; result pagination uses a bounded
signed-64-bit row ID, while scalar/block values remain fixed-width 256-bit BLOBs.

An existing v1 journal is validated and backed up to a private `pre-v2-UUID` child
directory before its schema changes. The writer reservation excludes concurrent
mutations while a separate read connection creates the sealed snapshot. Migration
and the new schema/version digest commit together. C13 fresh journals created both
migrations in one transaction; current fresh journals apply all migrations
through v7 together. Unsupported versions or changed migration hashes
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
process exit. Standalone `checkpoint run` permits one owner per local journal.
The [C20 supervisor](MULTI_GPU.md#ownership-and-failure-boundaries) uses a shared
journal guard and exclusive per-block guards for concurrent independent devices.
Allocation and inspection may still use concurrent connections. This is a
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
batch size without crediting its retained candidate prefix. C17 restores larger
xpoint batches after half-full or emptier successes, up to the configured limit;
full buffers retain a stable smaller size. This tuning does not change receipts
or the persisted interval union.

On restart, planning starts from the complement of committed intervals, not the
previous process's counters or stdout. Partial BSGS group cursors are intentionally
not persisted: replaying a bounded tile keeps restart independent of launch/group
geometry. With many targets, completing all groups of one tile can take longer
than the checkpoint cadence; scalar coverage cannot be accepted partway through
those groups. Reduce `--giant-batch` when a shorter tile replay window is needed.
A retry after the final commit performs no new GPU work. Expired or
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
This preserves replay/audit evidence but grows with actual checkpoints. The
coordinator bounds its pending outbox, but no automatic receipt-history archival
protocol is implemented. There is no second filesystem progress journal. Results and intervals live in the same SQLite commit.

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
that grant on one HIP or CUDA logical device. All actions accept `--state-dir` or the
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
| `create` | Required `--project`, `--mode xpoint\|bsgs\|hash160\|address\|ethereum\|vanity\|minikeys`, `--range`, `--block-width`, `--targets`; BSGS requires `--table` and accepts `--host-memory`; scalar families accept `--stride HEX` and `--order forward\|reverse` |
| `run`, common | Required `--backend hip\|cuda`, `--grant`, `--targets`; optional `--device` (0), `--candidate-capacity` (1024), `--checkpoint-seconds 0..60` (10) |
| `run`, xpoint | `--batch-size 1..1048576` (1048576), `--kernel stepped\|direct\|glv` (stepped); capacity 1..1048576 |
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

Use the CUDA binary and `--backend cuda` in place of the HIP binary/flag in
these examples for NVIDIA. CPU-only builds support creation, claims, checks and
result inspection; both HIP and CUDA execution requests fail explicitly. C14
adds CPU-compatible local control clients and graceful signal handling for `checkpoint run`. See the
[pause/resume guide](PAUSE_RESUME.md) for durable-pause acknowledgments, early-stop
summaries, live status and recovery. Ordinary volatile search commands retain
their existing signal behavior.

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

## Destructor collision found during validation

The first debug and sanitizer end-to-end runs failed while creating a BSGS job.
Two different classes had the same qualified name, `keyhunt::core::BsgsTargets`:
the C04 legacy loader aggregate (points plus a compression array) and the C11
canonical public-key target set (points plus a digest). This violates C++'s
one-definition rule across translation units. The sanitizer trace entered the
legacy aggregate's destructor for a canonical target object and attempted to free
the digest bytes as a pointer. Optimized release runs had passed, masking the
collision through different code generation.

The canonical type is now `BsgsPublicKeyTargets` throughout the GPU/search/storage
code. The legacy loader retains its type and behavior. A regression compiles both
headers together and asserts distinct types, and the new command test exercises
creation/destruction in release, debug and sanitizer builds. The original failures
and diagnosis are retained in [C13_ODR_FINDING.json](baselines/C13_ODR_FINDING.json).
This changes host type identity, not BSGS arithmetic or device kernel selection.

## Measured checkpoint cost

The [opt-in measurement](../tools/measure_c13_checkpoints.py) compares equivalent
no-match workloads on logical HIP device 0 after the regression suites finish:
2^20 scalars, one target, 65,536-scalar xpoint batches, or BSGS m=257 with 256
giants per tile and one target. One warm-up round is excluded; three fresh
processes per variant are retained with rotating variant order. All outputs
are checked for exact completion and no matches.

[Raw timing samples](baselines/C13_CHECKPOINT_TIMING.json) include binary/oracle
identity and the complete summaries. Process wall time includes startup,
input/table preparation, HIP initialization, execution, output and cleanup.
Checkpoint time measures the result/coverage SQL transaction, not preflight,
executor registration or final connection close/checkpoint.

| Mode | Persistence | Median process wall ms | Median checkpoint transaction ms | Commits |
| --- | --- | ---: | ---: | ---: |
| xpoint | volatile | 616.151 | 0.000 | 0 |
| xpoint | timed | 648.831 | 0.474 | 1 |
| xpoint | every-batch | 629.700 | 3.862 | 16 |
| bsgs | volatile | 673.707 | 0.000 | 0 |
| bsgs | timed | 688.186 | 0.473 | 1 |
| bsgs | every-batch | 692.437 | 3.781 | 16 |

For these short runs, the ten-second default coalesced all sixteen no-match
batches into one final commit. Per-batch mode used sixteen transactions.
Observed transaction time was about 0.47 ms with the default, versus 3.8–3.9 ms
with per-batch commits. Keep the default batching; found matches still commit
immediately. This avoids coupling every short GPU launch to a durable write.

Process timing includes substantial startup cost and run-to-run variation:
the xpoint per-batch median was lower than its timed-checkpoint median despite
more SQL transaction time. Three short samples do not establish a steady-state
GPU throughput difference. No GPU arithmetic, kernel selection, partition or
operating setting was changed for C13.

## Final acceptance

[C13_VALIDATION.json](baselines/C13_VALIDATION.json) retains source/schema/binary
hashes, raw CTest summaries, command reports, the eight-device inventory and
partition checks. The initial destructor-collision failures remain separately
documented rather than being hidden by the corrected runs.

| Build | Result |
| --- | --- |
| CPU release | 29/29 |
| CPU debug | 29/29 |
| CPU address/undefined sanitizers, focused selector | 27/27 |
| HIP release, gfx942 | 44/44 |

The focused sanitizer selector excludes only the pre-existing legacy
whole-application cpu_baseline and target_loading gates. Every new checkpoint
gate is included. Corrected builds completed without compiler warnings.
Documentation link/anchor/fence and whitespace checks also pass.

Real hardware validation uses MI300X SPX/NPS1. CPX/QPX/SPX discovery contracts
remain supported; live CPX/QPX searches were not performed on this configuration.
This section records C13 acceptance. C14 subsequently adds
[graceful controls, bounded drain and resume](PAUSE_RESUME.md). C15 now provides
[authenticated coordination, the atomic outbox and remote acceptance](COORDINATOR.md).

## C16 execution metrics

C16 introduced version 1 execution metrics; current summaries use
`metrics_version: 2`, device ordinal/UUID and
checkpoint cadence, successful `verified_device_steps`, HIP event `kernel_ms`
and `download_ms`, host `seed_ms`, executor `verification_ms` and
`executor_wall_ms`, owner `revalidation_ms`, discarded-attempt `replay_kernel_ms`,
total `download_bytes` (hexadecimal), and peak device/pinned allocation bytes.
Attempted work and timings include overflow; useful work excludes discarded
attempts. BSGS work is **target giant steps**, while `computed_scalars` counts
each completed all-target interval once. A completed retry reports no GPU work.

`preparation_ms` ends just before the first submission and includes journal
opening/auditing, input loading, discovery and lazy executor creation.
`executor_setup_ms` is the constructor subset; BSGS `table_upload_ms` is nested
within it. All three are zero when no executor is created. `wall_ms` spans journal
opening through execution and cleanup, before writing the final summary. Existing
`checkpoint_ms` measures `commit_search`, including its validation and SQLite
COMMIT; it is not a pure fsync measurement. Timing fields overlap as described and
must not be added together as disjoint phases. Failed processes have no complete
summary and must never contribute a throughput sample.

CPU receipt tests exercise overflow costs, useful-work counts, peak allocations,
transfer totals, BSGS target multiplicity, and completed-job retry metrics.

C17 fixes mixed BSGS dispatch reporting: `bsgs_groups` contains one record per
observed group (`group_size` 1 or 8), with `batches`, `overflow_replays`, attempted
`device_steps`, accepted `verified_device_steps`, and `kernel_ms`. Work counts are
hexadecimal strings. Group totals equal the summary totals, including discarded
overflow attempts. `bsgs_group_size` is retained for compatibility and means only
**the last dispatch**, zero when none ran. Xpoint and completed BSGS retries have
an empty group list. Benchmark readers label the complete group set unknown for
version 1 summaries; they cannot reconstruct it from the final group. See
[the C17 live reproduction](baselines/C17_GROUPS.json).

## Exact positive scalar strides

Create xpoint, HASH160/address, Ethereum or vanity jobs with `--stride HEX`.
The range still names private scalar bounds; block width counts visited
candidates. For example, range `1:301`, stride `3`, width `100` creates one
256-candidate block. `checkpoint run` recovers the persisted stride automatically;
an optional explicit stride must match. Changing batch size, kernel or visible
device does not change the progression or its saved complement.

Nonunit jobs use `scalar-stride-index-v1`. Saved/granted intervals are one-based
candidate indices, and summaries expose `computed_candidates`/`resumed_candidates`.
Result views include `candidate_index` and the actual private `scalar`. The
146-byte version-2 configuration binds A, B and S as three 32-byte big-endian
integers after the existing header. Its manifest root must be exactly `[1,N+1)`.
Schema 7 and receipt encoding are unchanged. `--stride 1` retains the old job
identity. BSGS and minikey jobs reject this option. See [contracts](C23_STRIDES.md)
and [executable examples](GPU_QUICKSTART.md).

## Exact reverse traversal

Create scalar-family jobs with `--order reverse`, optionally with `--stride HEX`.
Version 3 binds reverse order, original scalar bounds and positive stride. For
N candidates, index j maps to `A+(N-j)*S`; coverage advances through `[1,N+1)`
while scalars decrease. This applies at stride one too. The first scalar is the
last on-lattice value below the exclusive end. Runs recover order automatically;
an explicit conflicting order is rejected. Changing order requires a new job.

Public results show `candidate_index`, actual `scalar` and
`coordinate_space:"scalar-reverse-index-v1"`. Summaries count
`computed_candidates`/`resumed_candidates`. Generic grant/state intervals retain
their existing format and inherit the immutable mapping. Version-1/2 forward job
identities, schema 7, receipt encoding and ownership rules remain unchanged.
See [reverse contracts and recovery](C23_REVERSE.md).


## Exact-range GLV execution

Scalar jobs (`xpoint`, `hash160`/`address`, `ethereum`, `vanity`) accept
`checkpoint run --kernel glv`. The kernel computes the same public point for
each scalar; forward, strided and reverse jobs retain their existing bindings,
coordinates, receipts and schema. A restart may switch between `glv`, `direct`
and the default `stepped`. It resumes the committed complement of the same job.
BSGS uses its group selection; minikeys accepts only `direct`.
See [GLV arithmetic and compatibility](C23_GLV.md).
