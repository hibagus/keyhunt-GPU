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

## Fleet supervision

The Python supervisor starts one persistent native child per selected queue and
one independent scheduled-sync child. `--devices 0,1` selects a subset;
`--device-map QUEUE=ORDINAL` preserves queue identity after visibility changes.
`--rebind-device QUEUE` explicitly permits replacement with a different UUID only
after the prior process stops. One UUID cannot belong to two queue slots.
Unselected queues retain active ownership; their unstarted grants remain eligible
for same-job balancing. No process termination or completion triggers a sync.

`--host-memory` caps each device's table/target preparation budget, while
`--host-memory-total` divides an aggregate cap among selected devices. These are
allocation budgets, not total process RSS guarantees; runtime contexts, SQLite
and Python add overhead. Device table allocation also retains the backend's
free-memory and reserve checks. The supervisor never multiplies package HBM by
the number of logical partitions or changes partition modes.

Progress is a monotonically increasing completed-batch/checkpoint event, not log
file growth. Confirmed `paused` and `idle` states exclude the execution watchdog.
A draining or running child still has a deadline. A stalled device gets a bounded
30-second drain, then SIGKILL and quarantine; healthy devices and sync continue.
Ordinary device failures receive bounded retries and quarantine after three
failures. Typed coordinator/lease blocks do not increment that counter (A16).
Socket and signal pauses share the same authoritative state (A18). Fleet shutdown
signals all children before sharing one drain deadline. A surviving kernel-held
process retains its owner locks and cannot be replaced unsafely.

`coordinator_supervisor` passes deterministic watchdog deadline tests and actual
multi-process tests for simultaneous startup, one isolated memory failure,
bounded retries/quarantine, healthy completion, and one machine sync check.
The live HTTPS/HIP regression also passes with the concurrent supervisor.

## Work units and reference calibration

Supervised execution starts with one bounded unit, then sizes the next unit from
accepted scalar coverage and active wall time, targeting 180 seconds. Integer
geometry stays exact and each unit is clamped to its remaining block/gap.
Xpoint batches carry the parent work-unit identity. BSGS units align to `m` and
still credit a tile only after all targets finish. Timing includes validation,
all target groups, overflow attempts and periodic commits; local pause waits are
excluded. Kernel batch limits, ten-second checkpoints and machine sync cadence
remain independent. An estimate changes only future local units. Exact coverage
is journaled through the existing atomic checkpoint receipts; planned unit bounds
are also emitted as `work-unit` events.

For a new job, recommend a fixed width from at least five warmed, complete,
validated single-device measurements of the exact inputs:

```sh
python3 tools/calibrate_blocks.py --report docs/baselines/C20_FLEET.json \
  --mode xpoint --reference-device 0
```

The tool computes `align_up(ceil(R * 43200), alignment)` using rational integer
arithmetic, and a 180-second initial work-unit recommendation. The reference
queue is explicit; mixed UUIDs/targets/configurations, missing warm samples and
invalid timings are rejected. For BSGS, alignment is `m`; complete-target measured
wall time includes tile setup and verification. Effective scalar coverage/s and
actual target-giant steps/s are reported separately. Recommendations never modify
an existing job. Without a suitable reference, job creation still requires an
explicit `block_width`. Twelve hours is a prediction, not a twelve-hour test or a
completion-time guarantee for every device.

Exact sizing/alignment/overflow tests and checkpoint, pause, concurrent ownership
and live HTTPS/HIP regressions pass after adaptive sizing. Calibration tests
cover rational rounding, BSGS units and rejection of unmeasured/mixed inputs.

## Selected-device startup isolation (A20)

The concurrent C17–C19 audit identified full discovery in each worker self-test.
C20 now uses `select_gpu` for self-test metadata and runtime versions. HIP and
CUDA selection query only the requested ordinal and leave the owner thread on
that device. This also avoids restoring an untouched default CUDA ordinal, which
could initialize its context. Full `devices` inventory remains an explicit
all-device diagnostic operation.

The discovery mock passes SPX/QPX/CPX layouts and visibility remapping with device
0's memory query failing while selected device 1 succeeds. Only device 1 is
queried. Real HIP selected-device self-tests and two-device fleet/HTTPS regressions
pass. The CUDA path uses the existing selected-device API and version queries;
new NVIDIA hardware validation is unavailable on this host and is not claimed.
