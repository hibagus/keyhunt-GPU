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
