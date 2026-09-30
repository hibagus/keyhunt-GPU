# Graceful pause and resume (C14)

C14 extends the verified checkpoint owner with control boundaries before each
bounded submission. It keeps C13's one owner, one assigned block and one HIP
logical device per invocation. Multi-device supervision belongs to C20.

## Owner contract

A pause stops subsequent submissions, drains the current bounded runner, verifies
its completion and forces pending exhaustive coverage into a local transaction.
Only after COMMIT does the owner report durably paused. The journal's primary
state remains `in_progress`; ownership, deadline and executor lock remain held.
Running/paused are process activity, not new allocation states.

A no-match pause commits early even if the normal ten-second timer is not due.
An overflow is never credited. BSGS subgroup matches are already committed
immediately, but an incomplete all-target tile has no scalar coverage. A live
pause retains that subgroup cursor in memory; a process restart replays the
incomplete tile and deduplicates matches.

Resume audits the saved journal and checks assignment epoch, owner, generation,
deadline and executor generation before another launch. Pause does not renew a
deadline. Expired or transferred assignments reject resume; explicit recovery is
required after the old executor stops. No network is needed for a valid local
resume.

Graceful stop uses the same forced checkpoint, releases the executor only after
cleanup, and returns a summary with `complete=false` unless the block finished.
A pause racing with the final completed batch reports completion and exits.
Errors during verification or commit do not acknowledge a durable pause.

No schema change is necessary: schema 2 already stores exact coverage, matches,
fences and immutable bindings. Activity status is only meaningful while the
exclusive process is alive. Existing supported SQLite online backup and sealed
restore APIs preserve the pause frontier; restored copies remain quarantined.

## Validation in development

`storage_checkpoint_control` exercises no-match checkpoint forcing, repeated
pause/resume, idle ownership retention, graceful stop/restart, expiry and transfer
during pause, BSGS partial-target live resume and restart, overflow replay, final
batch completion and online backup/restore while paused. Existing checkpoint
fault tests continue to exercise abrupt process exits around transaction and
acknowledgment boundaries.

## Commands and signals

Start a normal [checkpoint run](CHECKPOINTS.md). In another
terminal, use the same private state directory:

```sh
keyhunt checkpoint pause  --state-dir "$state"
keyhunt checkpoint status --state-dir "$state"
keyhunt checkpoint resume --state-dir "$state"
keyhunt checkpoint stop   --state-dir "$state"
```

The command response has `accepted`, `state`, `requested`, `durably_paused`,
`durability:"local"`, process ID, project/job/block, assignment generation, selected
logical device ordinal and visible-device count. Pause/stop acceptance only
means the owner accepted the request. Wait for `state:"paused"` with
`durably_paused:true` before treating a pause as a checkpoint. A resume response
can say `resuming` while integrity and ownership checks run. Rejected commands
return `accepted:false` and exit 2. Resume during a pending drain is rejected;
repeat it after the durable pause. Repeating pause while paused is harmless.

The run's NDJSON also emits control transitions: `draining`, `paused`, `running`
(after resume), `stopped` or `completed`. Existing checkpoint records retain
their post-COMMIT meaning. The final summary's `complete` applies to this assigned
block and is false after an early graceful stop; a successful stop exits 0.

| Signal | Action |
| --- | --- |
| `SIGUSR1` | Request pause |
| `SIGUSR2` | Resume a durably paused owner |
| `SIGINT` or `SIGTERM` | Checkpoint and exit |
| Second `SIGINT`/`SIGTERM` | Force exit at the next owner control boundary, exit 128 + signal |

Handlers only set `sig_atomic_t` flags. Force exit runs in the owner loop, so
uncommitted work replays. Standard Unix signals can coalesce: two simultaneous
signals are not guaranteed to be two delivered interrupts. If a driver is stuck
inside a call, use `SIGKILL` for immediate process termination. `SIGSTOP` suspends
the process without creating a checkpoint.

Control is local and requires neither network access nor coordinator contact.
The private `control.sock` uses Linux Unix-domain `SOCK_SEQPACKET`, same-UID peer
checks, a 0700 journal directory and a 0600 socket. The exclusive journal owner
creates it lazily after preflight. It removes its own socket before releasing the
executor lock; restart under that lock removes a stale socket after a crash.
Symlinks or unexpected files at that path reject startup. The complete socket
path must fit Linux's 107-byte pathname limit.

Clients have a five-second response timeout. A timeout can mean a request is
queued behind an admitted batch or a slow commit; it does not cancel the request.
Inspect status before retrying. The owner bounds pending clients and expires
silent clients; disconnected clients cannot block output or cause SIGPIPE.

## Drain latency and execution cost

Controls are serviced between bounded synchronous batches, including overflow
retries and each BSGS target group. At most one admitted batch remains before the
owner can force the local checkpoint. The bound is work, not a universal
millisecond deadline: GPU execution, candidate verification, filesystem sync and
driver stalls determine wall time. Reduce `--batch-size`, `--giant-batch` and
`--target-batch` to reduce this bound. GPU kernels and their existing blocking
completion path are unchanged; the running owner adds a nonblocking control
check per batch, with no fixed sleep or per-batch forced fsync.

Idle owners wait in 20 ms increments and retain device allocations and the lock.
A pause received during startup can include initialization and one admitted batch.
The reported `pause_ms` measures owner observation of the request through the
durable pause; the remaining GPU batch before observation is excluded. External
request-to-paused measurements include that wait and are the operational latency
to use. Paused time and resume integrity auditing are outside GPU throughput.

## Inspection, backups and recovery

`checkpoint status` is live activity, not persistent evidence that a dead process
paused. If there is no reachable owner, inspect the journal:

```sh
keyhunt state check --state-dir "$state"
keyhunt state block --state-dir "$state" --project "$project" --job "$job" --block 0
keyhunt checkpoint results --state-dir "$state" --project "$project" --job "$job"
keyhunt state backup --state-dir "$state" --destination "$snapshot"
keyhunt state restore --state-dir "$restored" --source "$snapshot"
```

Online backups use SQLite's supported backup API, include committed WAL data,
and publish a sealed snapshot with a new epoch. Do not copy the live main file
alone. A restored snapshot supports inspection but remains quarantined; C15's
coordinator reconciliation is still needed to activate recovered ownership.

Restart `checkpoint run` with the existing valid grant to consume the exact
saved complement. If expired or transferred, stop the old owner and use the
explicit `state recover` operation from [the storage guide](STORAGE.md), then use
the returned grant. Pause itself never returns a block or silently claims one.

No persistent schema migration is added in C14. C13's v1-to-v2 migration still
keeps a sealed pre-migration snapshot; its checksum and recovery tests remain
acceptance gates. Running activity is intentionally not a durable allocation
state. Changing device ordinal, visible-device count or compatible batch geometry
does not change the bound job; targets, algorithm and BSGS table must still match.

## Hardware recovery gate

The `checkpoint_pause_hip` integration test uses pinned libsecp256k1 targets and
real xpoint/BSGS executors. It pauses repeatedly, verifies an unchanged frontier
while idle, checks online backup and quarantined restore, exits via SIGINT and
SIGTERM, then finishes the exact remaining range with changed launch geometry.
Every expected boundary match must appear exactly once.

On an unrestricted host with at least three logical devices, the same grant
moves from visible devices `0,1` (ordinal 1), to `1` (ordinal 0), to `0,1,2`
(ordinal 2). This tests fewer/more visible devices and a different physical GPU
without changing logical job identity. Restricted or single-device environments
retain their caller's visibility and record that count-change coverage was not
exercised. Partition modes are not changed by this test.
