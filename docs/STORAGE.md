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
sealed with a fresh epoch and quarantine flag, synced and published exclusively.
Existing destinations are never replaced. A crash before publication leaves no
usable destination; a directory-sync failure after publication can leave a
complete sealed snapshot while reporting an error. Never copy only a live main
database file or delete its WAL manually.

Restoring validates the source and creates another exclusive snapshot with a
new epoch. Restored/backup databases permit inspection but reject allocation and
coverage writes. They cannot silently reactivate grants that a newer live journal
may already have reassigned. Reconciled activation and old-backup recovery policy
belong to C15; quarantine is not a claim that an offline GPU has been stopped.
