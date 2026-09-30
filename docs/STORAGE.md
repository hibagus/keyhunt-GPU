# Sparse local journal (C12)

C12 supplies local storage primitives for project-scoped jobs, sparse block
selection, assignments and exact accepted coverage. Search execution is still
separate: C13 must commit verified results with coverage before a GPU receipt
becomes durable progress. This library is not the authenticated coordinator;
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

The initial migration is `src/storage/schema_v1.sql`. A private application ID,
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
files or table caches. C13 must resolve and bind those inputs before execution.
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
work. The CLI does not expose arbitrary coverage writes. C13 will supply the
verified-result/coverage commit boundary; C09/C11 search output remains volatile.

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
