# C20: concurrent machine execution

This document records C20 implementation decisions and validation. C20 remains
in progress until its acceptance evidence is recorded here.

## Ownership and failure boundaries

The coordinator grants work to a machine instance. Its two-hour synchronization
schedule and thirty-day lease remain unchanged. Local dispatch chooses a distinct
logical block for each device. GPU execution and HTTPS remain separate processes.

Standalone checkpoint execution retains exclusive ownership of `executor.lock`.
Supervised execution takes a shared lock on the same stable inode and an exclusive
Linux open-file-description byte-range lock derived from project, job and block.
This permits different blocks concurrently and excludes duplicate execution even
within one process. A hash collision conservatively denies concurrent execution.
Locks release on descriptor close or process death; lock files are never unlinked.
The existing assignment and checkpoint executor generations remain authoritative.

A crashed or hung owner must stop before its block can resume. GPU reset is not a
recovery mechanism. Durably accepted coverage survives; uncommitted work replays.
Lease expiry, uncertain offline deadlines, coordinator pauses and stale fences
have typed `ExecutionBlocked` reasons, so expected control transitions need not
count as hardware failures (C15 audit A16).

## Validation ledger

- `storage_checkpoint_concurrent`: a separate process holds one block inside a
  simulated hung GPU submission. Another block completes, duplicate and standalone
  execution are refused, then the killed owner's block replays. The final journal
  audit and four exact CPU-verified boundary matches check coverage and results.

## Local dispatch (schema v6)

`Worker::acquire_device` holds a private, stable per-queue process lock and binds
that queue to a reported device UUID. A changed UUID requires an explicit rebind;
the lock prevents rebinding while the old process remains alive. The saved slot
retains its unfinished block across restart or replacement. This is a local
handoff of the existing machine lease, not a new coordinator assignment.

`Worker::claim_device` uses `BEGIN IMMEDIATE` and a unique project/job/block key.
It resumes the saved active block first, otherwise prefers its own queued grants
and may take an unstarted, unclaimed grant from another queue for the same job.
A paused or expired active block stays in its slot awaiting revalidation. A
finished slot is cleared before selecting another block. Server queue metadata
is retained independently, so lease renewal cannot overwrite local ownership.
Schema migration retains the normal sealed pre-upgrade backup and checksum audit.

`coordinator_dispatch`, `coordinator_worker`, and `storage_checkpoint` pass with
schema v6. Tests cover fast-device queue stealing beside a held slow-device
block, duplicate process exclusion, idempotent claims, stopped-device UUID
replacement, pause/expiry gates, retained outbox data and migration recovery.

## Persistent execution and local controls

`keyhunt-worker run-device` owns one device slot for its lifetime and performs a
fresh GPU self-test in that process. Immutable targets, the verifier, host table
and drained GPU allocations survive normal block handoffs. Each new grant still
passes the journal audit, binding checks, offline deadline and executor generation
allocation. An exception during submission destroys/drains the affected executor
before releasing the block guard. Expected lease/control exceptions emit a typed
`blocked` event and retain the process while waiting for revalidation.

The persistent endpoint is `control-QUEUE.sock`. Use
`keyhunt checkpoint pause|resume|stop|status --state-dir DIR --slot QUEUE`.
Signals retain their previous meanings. Standalone execution still uses
`control.sock`. A queue's process lock protects its persistent endpoint; a block
lock protects each individual checkpoint run. Control intent survives handoff.

The live `coordinator_https_worker` HIP/mTLS test passes: two short BSGS grants
complete with exactly one executor setup/table upload and one fresh self-test.
Their outbox stays pending until explicit synchronization. Existing
`checkpoint_controls` socket, signal, durability and recovery tests also pass.
This addresses the repeated initialization mechanism identified by audit A17;
end-to-end fleet measurements are recorded separately below.
