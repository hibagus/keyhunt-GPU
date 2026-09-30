# Sparse local journal (C12)

C12 supplies local storage primitives for project-scoped jobs, sparse block
selection, assignments and exact accepted coverage. C13 now adds
[verified GPU checkpoint integration](CHECKPOINTS.md), committing matches with
accepted coverage through a separate durable command. This library is not the authenticated coordinator;
C15 must enforce project membership and derive owner identity from credentials.

## Database and deployment boundary

State defaults to `$KEYHUNT_STATE_DIR`, then `$XDG_STATE_HOME/keyhunt`, then
`$HOME/.local/state/keyhunt`. An explicit state directory overrides those defaults.
Paths must be absolute, outside the source checkout and every enclosing Git
checkout, including through symlinks. Runtime directories are owned by the caller
and private (0700); databases are private regular files (0600), without hardlinks
or symlinks. Existing directory permissions are checked, not silently changed.
The database name is fixed as `progress.sqlite`; SQLite owns its adjacent WAL/SHM.
Accidental journal files are also ignored by Git.

Connections enable foreign keys, WAL, a five-second busy timeout and
`synchronous=FULL`. Selection and writes use `BEGIN IMMEDIATE`, so contention
cannot produce two owners for the same block. The deployment boundary is local
storage on one host; remote workers must eventually use the coordinator API.
SQLite documents these [WAL concurrency/durability rules](https://www.sqlite.org/wal.html)
and [transaction behavior](https://www.sqlite.org/lang_transaction.html).

SQLite 3.51.3 or newer is required at configuration and runtime. This deliberately
includes the upstream [WAL-reset race fix](https://www.sqlite.org/wal.html#walreset).
The C12 host lacks system development headers and has an older system runtime;
validation uses the already installed standalone SQLite 3.51.3 library and header
under ROCm's `rocm_sysdeps`. It requires no GPU API, GPU discovery or network fetch.
Other installations may supply an ordinary SQLite development package meeting the
version requirement. Explicit paths for this host are:

```sh
cmake --preset cpu-release \
  -DSQLite3_INCLUDE_DIR=/opt/rocm/core-10.0/lib/rocm_sysdeps/include \
  -DSQLite3_LIBRARY=/opt/rocm/core-10.0/lib/rocm_sysdeps/lib/librocm_sysdeps_sqlite3.so
```

## Schema and migration

The immutable initial migration is `src/storage/schema_v1.sql`. C13 upgrades to
[schema version 2](CHECKPOINTS.md#schema-version-2-and-migration) with an automatic
sealed pre-migration backup; the v1 contract below records the C12 foundation. A private application ID,
`user_version=1`, and SHA256 of the exact migration text identify the schema.
Only an empty, unowned database is initialized. Foreign databases, unknown
versions and changed migration checksums fail without reinterpreting state.
Future changes require a new migration, never editing a published migration.

Projects are opaque generated UUIDs. Jobs have composite `(project,job)` keys.
Assignments, partial coverage, completed runs, selection nodes, retry receipts and
events carry the project/job scope and composite foreign keys. Fixed-width
32-byte big-endian BLOBs represent wide unsigned values, keeping SQLite byte
ordering equivalent to integer ordering. Signed SQLite integers are limited to
bounded generations, timestamps and local counts, never scalar/block IDs.

## Backup and restore boundary

Backups use the [SQLite online backup API](https://www.sqlite.org/backup.html),
including committed WAL content. They are written to a private staging file,
sealed with a fresh epoch and quarantine flag, switched to DELETE journal mode,
synced and published exclusively as a self-contained file with no WAL dependency.
Existing destinations are never replaced. A crash before publication leaves no
usable destination; a directory-sync failure after publication can leave a
complete sealed snapshot while reporting an error. Never copy only a live main
database file or delete its WAL manually.

Restoring validates the source and creates another exclusive snapshot with a
new epoch. Restored/backup databases permit inspection but reject allocation and
coverage writes. They cannot silently reactivate grants that a newer live journal
may already have reassigned. Reconciled activation and old-backup recovery policy
belong to C15; quarantine is not a claim that an offline GPU has been stopped.

## Sparse selection and identity

A v1 job manifest is exactly 167 bytes: `khjob`, version byte 1, mode byte
(1=xpoint, 2=BSGS), then five 32-byte fields: root begin, exclusive root end,
block width, target digest and algorithm/configuration digest. Its SHA256 is the
job ID within a project. The semantics are secp256k1 scalar ranges, stride one,
all targets and exhaustive completion. Hardware and launch geometry are excluded.
The supplied digests describe intended inputs; C12 does not load/verify GPU target
files or table caches. The C13 checkpoint commands resolve and bind those inputs
before execution.
Creating the same job again preserves its existing selection seed/counter.

The free-space index is a binary tree over `[0, block_count)`, split at integer
midpoints. An absent node represents a wholly unexplored subtree; a zero-free
node represents a wholly reserved/finished subtree. Partial nodes store exact
256-bit free counts. Selection descends by rank, while claims/returns change only
the selected path. Fully occupied subtrees collapse to one row; wholly free ones
collapse to no rows. Untouched enormous jobs have zero index rows. Cost scales
with touched fragmentation and at most 256 path levels, not theoretical blocks.

- Sequential selection takes the lowest free rank.
- Random selection draws a rank over the exact global free count. SHA256 of a
  domain tag, persisted 256-bit seed and checked counter supplies deterministic
  random bytes; masked bounded rejection removes numeric-tail bias. It never
  repeatedly guesses occupied IDs, including at exhaustion.
- Random-window selection first draws a global free rank, then selects within
  that block's fixed-width ID window. A request returns at most the free blocks
  in that window; it does not silently refill from other windows. This is a
  clustered policy, not globally uniform sampling without replacement.
- Manual selection accepts exactly one unexplored ID and otherwise reports a
  conflict. Out-of-grid IDs are rejected without allocating rows.

One claim reserves 1..256 blocks. Its selection counter, grants, audit event and
request receipt commit together. Idempotency is scoped by project, job, owner,
operation and request key; a changed payload under that key is rejected. A retry
returns the original grants/deadlines, even after their release or expiry, without
allocating more blocks. Such historical receipts do not authorize a new execution;
the current owner/generation/epoch and expiry must still be checked. Empty results
are also retained: use a new request key for a new allocation attempt.

## Assignment and coverage lifecycle

Blocks have exactly three primary states: unexplored, in_progress and finished.
Queued, started and expired are metadata. Default assignments last thirty days;
explicit lifetimes are 1..2,592,000 seconds. The repository obtains time from its
trusted clock; only tests inject a synthetic clock. Expiry never frees a block.
Paused/offline assignments therefore cannot be selected by another ordinary claim.

A monotonically increasing per-job signed-64-bit generation fences ownership.
Exhaustion is rejected rather than wrapped. Return/reclaim and explicit recovery
receive fresh generations, including when the owner string stays the same.
Renewal preserves the generation and never shortens the current deadline. Recovery
preserves accepted partial coverage; before expiry it requires the caller's
explicit confirmation that the previous executor stopped. Fencing cannot itself
stop a GPU. Only valid, provably unstarted spares with no coverage can be returned.
Finished blocks have no normal reopen/reset operation.

The trusted `record_coverage` API accepts 1..1024 exact intervals for a started,
current assignment. It checks containment, merges overlapping/adjacent intervals
and records the receipt/event in the same transaction. Full exact coverage removes
partial rows and the active assignment, then coalesces adjacent finished block
runs. Partial blocks remain in_progress; their complement identifies unsearched
work. The CLI does not expose arbitrary coverage writes. C13 supplies the
[verified-result/coverage commit boundary](CHECKPOINTS.md) for bound search jobs;
raw `record_coverage` now rejects those jobs. The ordinary C09/C11 commands remain
volatile; use `keyhunt checkpoint run` for durable searches.

`check()` audits SQLite integrity and foreign keys, canonical job identity,
coverage containment, disjoint block states, and every persisted subtree count
against independent prefix sums of authoritative assignments/finished runs.
`compact()` checkpoints and vacuums physical storage without deleting retry
receipts or audit history. Statistics expose file/WAL bytes, free pages and row
counts. Retention/archival policy is future coordinator work; sparse state does
not promise constant space for arbitrarily fragmented work or unlimited retries.

## Repository validation

The initial release tests exhaust 254 tiny occupancy patterns, every free rank
and every bounded window. They cover collapse/re-expansion, wide block IDs above
240 bits, near-exhaustion random selection, target/configuration identity,
project separation, interval merging/complements, expiry, renewal/recovery,
fencing, retained retry deadlines, completed-run coalescing and compaction.
A semantic index-corruption fixture is rejected by the independent audit.

The separate fault binary exits real child processes immediately before and after
COMMIT, then verifies rollback or the original durable receipt on retry. Sixteen
writer processes test disjoint concurrent allocations and concurrent retries of
one request. These are process-crash checks, not a simulated host power failure
or a certification of the underlying storage hardware. Production binaries have
no transaction fault hook.

## Local commands

Every action prints one JSON object. Failures print a diagnostic to stderr and
exit 2. State operations are CPU-only in both CPU and HIP builds. For example:

~~~sh
export KEYHUNT_STATE_DIR="$HOME/.local/state/keyhunt-c12"
./build/cpu-release/keyhunt state init
./build/cpu-release/keyhunt state project-create --name "Local project"
~~~

Use the returned UUID as `PROJECT`. A synthetic job can then be registered using
explicit input/configuration digests; these sample digests do not identify real
target files:

~~~sh
./build/cpu-release/keyhunt state job-create --project "$PROJECT" \
  --mode xpoint --range 1:10001 --block-width 100 \
  --target-digest 1111111111111111111111111111111111111111111111111111111111111111 \
  --algorithm-digest 2222222222222222222222222222222222222222222222222222222222222222
~~~

Use the returned 64-digit ID as `JOB`. Numeric range endpoints, block widths,
block IDs and window widths are hexadecimal; ranges are half-open. Counts,
lifetimes and generations are decimal. Wide output values are fixed-width hex
strings; generation numbers are decimal strings to avoid JSON integer truncation.

~~~sh
./build/cpu-release/keyhunt state claim --project "$PROJECT" --job "$JOB" \
  --owner workstation --request allocation-001 --policy sequential --count 8
./build/cpu-release/keyhunt state inspect --project "$PROJECT" --job "$JOB"
./build/cpu-release/keyhunt state block --project "$PROJECT" --job "$JOB" --block 0
./build/cpu-release/keyhunt state check
~~~

The claim response includes a `grant` token for each assignment. Store the entire
token as `GRANT`; it contains project, job, owner, epoch, generation, expiry and
block ID. It is local fence data, not a password or authenticated credential.

~~~sh
./build/cpu-release/keyhunt state renew --grant "$GRANT" --request renewal-001
./build/cpu-release/keyhunt state return --grant "$GRANT" --request return-001
./build/cpu-release/keyhunt state recover --project "$PROJECT" --job "$JOB" \
  --block 0 --owner workstation --request recovery-001 --previous-stopped yes
~~~

Use `--previous-stopped yes` only after the previous executor has actually stopped.
An expired assignment can be explicitly recovered without this flag. Recovery
retains its accepted coverage and creates a fresh generation. Returning requires
an unstarted spare; C12's commands cannot start a search or credit coverage.

| Action | Additional options |
| --- | --- |
| `init` | None |
| `project-create` | Required `--name` |
| `job-create` | Required `--project`, `--mode xpoint\|bsgs`, `--range`, `--block-width`, `--target-digest`, `--algorithm-digest`; optional fixed 64-digit `--seed` for reproducible selection |
| `claim` | Required `--project`, `--job`, `--owner`, `--request`; optional `--policy sequential\|random\|random-window\|manual`, decimal `--count 1..256`, `--lifetime 1..2592000`; manual requires `--block` and count 1; random-window accepts `--window` (default hex 1000) |
| `inspect` | Required `--project`, `--job`; reports state counts and physical storage statistics |
| `block` | Required `--project`, `--job`, `--block`; reports assignment and exact coverage/complement |
| `renew` | Required `--grant`, `--request`; optional `--lifetime` |
| `return` | Required `--grant`, `--request` |
| `recover` | Required `--project`, `--job`, `--block`, `--owner`, `--request`; optional `--previous-stopped yes\|no`, `--lifetime` |
| `check`, `compact` | None; operate on the whole journal |
| `backup` | Required absolute `--destination` directory |
| `restore` | Required absolute `--source` directory and explicit destination `--state-dir` |

All actions accept `--state-dir` to override the environment. Duplicate/unknown
options and options belonging to another action fail. Owner and request tokens
are 1..128 ASCII letters, digits, `-`, `_`, `.`, or `@`. Project names are 1..256
printable ASCII characters. Digests are exactly 64 hex digits, without `0x`.
Initialization/project creation are local setup operations; project creation
generates a new UUID on every call. Allocation/lifecycle operations require a
request key; retry the exact original payload and key after uncertain output.
An output failure occurs after a successful mutation may already be committed.

~~~sh
./build/cpu-release/keyhunt state backup --destination /absolute/private/snapshot
./build/cpu-release/keyhunt state restore --source /absolute/private/snapshot \
  --state-dir /absolute/private/restored
./build/cpu-release/keyhunt state check --state-dir /absolute/private/restored
./build/cpu-release/keyhunt state compact
~~~

Backup/restore destinations must not already contain a database. A restored
journal is inspectable but cannot allocate, renew, recover, return or accept
coverage. There is deliberately no activation override in this milestone.

The CLI regression uses an independent Python manifest encoder and SQLite reader,
checks project foreign keys, all four policies, simultaneous request retries,
lost stdout via `/dev/full`, sealed restore, environment precedence and high-bit
IDs. Run the storage gates with `ctest --preset cpu-release -L storage`.

## Storage measurements and decisions

The [reproducible measurement](../tools/measure_c12_storage.py) uses three fresh
synthetic journals per case, a fixed selection seed and one 256-block claim.
The raw samples and row/file statistics are retained in
[C12_STORAGE_SCALING.json](baselines/C12_STORAGE_SCALING.json). Timings include
CLI startup, schema validation, FULL transaction commit, JSON and connection
close/checkpoint. These are shared-host observations (regressions were also
running), not an isolated transaction-throughput benchmark or GPU measurement.

| Theoretical blocks | Policy | Median claim wall ms | Index rows | Compacted DB bytes |
| --- | --- | ---: | ---: | ---: |
| 2^20 | sequential | 119.13 | 13 | 131,072 |
| 2^20 | random | 98.99 | 3,346 | 626,688 |
| 2^20 | random-window | 134.12 | 1,343 | 331,776 |
| 2^255 | sequential | 1347.48 | 248 | 167,936 |
| 2^255 | random | 887.63 | 63,529 | 9,441,280 |
| 2^255 | random-window | 2082.07 | 1,578 | 364,544 |

Every untouched job used zero index rows, including the 2^255-block case.
Statistics are per-job row counts but whole-database file/WAL sizes; each
measurement database contains only one project/job. Post-command WAL sizes
are zero because the last connection has closed and checkpointed; these
numbers do not estimate peak WAL size with long-lived readers. Raw before/
after-compaction statistics preserve the physical-size distinction.

The 255-bit random case occupied 63,529 tree rows for 256 scattered IDs;
a random window of 4,096 blocks reduced this to 1,578. Window selection was
slower here because each draw computes window ranks through the tree. Keep
sequential as the default and expose random-window for users who want bounded
fragmentation; do not present it as uniform global random selection or a
speed improvement. This allocation occurs per logical block reservation,
outside the GPU kernel loop. No GPU throughput improvement is claimed by C12.

Decision: retain the tested sparse tree and exact rank selection. Defer bitmap
pages, prepared-statement caching and transaction-throughput tuning until a
representative coordinator workload establishes the relevant bottleneck.
Generation/request history is retained through compaction; bounded retention
needs an explicit retry/archival protocol, not deletion based only on age.

## Final C12 acceptance

[Validation evidence](baselines/C12_VALIDATION.json) records implementation commits,
schema/source/binary/library hashes, raw CTest summaries, CLI reports, hardware
inventory and the partition regression. The four new storage gates pass in each
build:

| Build | Result |
| --- | --- |
| CPU release | 26/26 |
| CPU debug | 26/26 |
| CPU address/undefined sanitizers, focused selector | 24/24 |
| HIP release, gfx942 | 41/41 |

All four incremental builds completed without compiler warnings. Documentation
links/anchors/fences and whitespace checks pass. The sanitizer selector excludes
`cpu_baseline` and `target_loading` because of previously recorded legacy
whole-application defects; it includes every new storage gate and the state CLI.

The hardware suite retains the C07–C11 tests across the eight visible MI300X
SPX/NPS1 devices, including xpoint, BSGS and partition contracts. CPX/QPX discovery
contracts pass; actual CPX/QPX hardware search runs remain pending. C12 introduces
no kernel changes and does not modify GPU partitions or operating settings.

C12 is complete. [C13](CHECKPOINTS.md) now binds canonical inputs and verified
matches/coverage to durable execution, checks stale generations and replays
uncommitted work. Its separate checkpoint commands consume C12 assignments;
the ordinary C09/C11 commands keep their volatile semantics.
