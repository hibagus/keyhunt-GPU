# C20: concurrent machine execution

C20 is complete for the validated HIP and native CUDA localhost scope. The
[HIP acceptance manifest](baselines/C20_VALIDATION.json) and
[H200 follow-up](C20_CUDA_VALIDATION.md) record independent source/binary hashes,
raw tests, 1/2/4/8-GPU measurements and lifecycle checks. Each backend retains its
own recorded hardware, toolchain and timing results.

## Ownership and failure boundaries

The coordinator grants work to a machine instance. Its two-hour synchronization
schedule and thirty-day lease remain unchanged for HTTPS workers.
[C22 file-only workers](OFFLINE_ASSIGNMENTS.md) use manual courier exchanges
without a scheduled-sync child. Local dispatch chooses a distinct
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

The live `coordinator_https_worker` HIP and CUDA mTLS tests pass: two short BSGS grants
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

BSGS `--tile-order both-ends` alternates low/high tiles within each grant. Up to
two contiguous work units may have interleaved submissions; when the frontiers
meet, both ends can consume one unit. Each unit adapts from its own active time,
while elapsed completion can include opposite-end work. Checkpoint coverage and
block claims remain exact and independent of this policy. See
[both-ends contracts](C23_BSGS_BOTH_ENDS.md).
Dance cycles low/high/middle with up to three interleaved work units. The fixed
midpoint bounds fragmentation; no tile or unit crosses it. Each unit retains its
own timing, and a resumed grant reconstructs the midpoint from saved coverage.
See [dance contracts](C23_BSGS_DANCE.md).

Minikey workers accept `--ordinal-order reverse`, selecting contiguous work units
and batches from the highest uncovered ordinal downward. Each device retains
its target allocation across grants. Restart may switch direction without
changing job identity or exact receipts. See [reverse minikey contracts](C23_MINIKEYS_REVERSE.md).

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
pass. The native `coordinator_cuda_contexts` gate now verifies the production
self-test on H200: eight GPUs are visible but only selected ordinal 7 has an
active primary context. Remapped visibility also passes; one/zero visible GPUs
explicitly skip this isolation gate. See [CUDA acceptance](C20_CUDA_VALIDATION.md).

## Bounded operational output

The owner observes every returned batch but emits progress at most four times
per second; checkpoints/control transitions remain immediate. This avoids making
fast kernels spend their time serializing per-batch diagnostics. Progress still
comes only from completed work, so throttling cannot hide a hung submission.
Each device log rotates on record boundaries at 8 MiB and retains one prior
file. Durable matches/coverage remain in SQLite and the outbox, not these logs.
A CPU process test feeds over 10 MiB of diagnostics and verifies rotation,
independent healthy completion, and aggregate memory-budget division. The real
two-device xpoint/BSGS fleet test passes with throttled output.

A shutdown review also covered driver-held processes that survive SIGKILL.
The main loop now exits after its shared drain deadline instead of waiting
indefinitely for such a PID. Reaping shares a further five-second budget across
the fleet, surviving owners are visibly quarantined, and their OS locks prevent
replacement. A deterministic 64-child test proves the maximum shared wait is
35 seconds, without assuming SIGKILL can interrupt a kernel driver.

The full CPU/coordinator run found one stale CLI fixture expecting schema v5.
The fixture and storage guide now expect v6; its independent foreign-key and
checkpoint-payload checksum checks remain intact. The failed gate then passed.
The other 46 CPU/coordinator gates passed in the full run; focused debug (9) and
ASAN/UBSAN (7, leak detection and halt-on-error enabled) checks also passed.

The full HIP run exposed three CLI failures in the empty-device-visibility case:
selected HIP discovery returned a runtime error instead of the established
`not visible` diagnostic. Selection now treats `hipErrorNoDevice` as an empty
inventory, while other runtime failures still propagate. A dedicated discovery
assertion and all three CLI gates pass. Together with the other 63 full-run gates,
all 66 HIP/coordinator gates are validated; no failures are waived.

## Measured fleet scaling

[Raw repeated measurements](baselines/C20_FLEET.json) cover eight physical MI300X
SPX/NPS1 devices (304 CUs each), gfx942 with the opt-in C19 carry path enabled.
No partition settings were changed. Each mode/device-count pair has one warm-up
run and five measured runs. Every run executes two nominal `2^32`-scalar blocks
per GPU, with a seven-scalar final tail reduction. All 48 runs pass: every selected
GPU completes work, all 360 block completions have exact per-job coverage, the
known scalar-1 match survives local durability and authenticated upload, and no
completion triggers an early upload. Each working process constructs one search
executor, reusing its targets and BSGS table across grants.

Times include fresh process startup, self-tests, durable GPU execution and
supervisor shutdown. Assignment setup and explicit post-run inspection/upload
are outside the measured interval. Rates are median finite-job throughput,
not a claim about twelve-hour sustained throughput or ideal linear scaling.

| GPUs | Xpoint seconds, median [min–max] | Xpoint billion scalars/s | Relative to one GPU | BSGS seconds, median [min–max] | BSGS billion effective scalars/s | Relative to one GPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4.343 [4.294–4.583] | 1.978 | 1.00× | 1.940 [1.933–1.945] | 4.428 | 1.00× |
| 2 | 4.583 [4.561–4.813] | 3.749 | 1.90× | 2.143 [2.127–2.155] | 8.017 | 1.81× |
| 4 | 5.002 [4.939–5.174] | 6.869 | 3.47× | 2.534 [2.528–2.544] | 13.557 | 3.06× |
| 8 | 5.753 [5.674–5.871] | 11.944 | 6.04× | 3.265 [3.211–3.325] | 21.048 | 4.75× |

Xpoint uses one X target. BSGS uses `m=257` and two signed public-key targets;
its effective scalar rate is not interchangeable with direct-scan work. Raw
records retain actual target-giant counts and per-grant wall/kernel times. These
small tables and finite jobs do not establish scaling for large tables, different
target counts, CPX/QPX, NVIDIA devices, or uncontrolled concurrent workloads.

The [reference recommendations](baselines/C20_CALIBRATION.json), derived from
five warmed single-device grants on queue 0, are **110,289,156,413,738 scalars**
for xpoint and **404,442,914,333,361 effective scalars** for this BSGS configuration.
These widths predict twelve active hours on that reference; they are neither
powers of two nor measured twelve-hour completions. Different inputs require
new calibration. Existing jobs never change their width.

An initial measurement attempt hit the coordinator's 120-request/minute limit
because the fixture explicitly inspected/uploaded many small jobs. The fixture
now paces those calls within the existing limit. Production synchronization was
not relaxed. The incomplete attempt is excluded from this measured dataset.

Reproduce the measurements and calibrated lifecycle check:

```sh
python3 tools/validate_fleet.py --build-dir build/hip-release \
  --apache-root /path/to/private/apache-root --counts 1,2,4,8 \
  --repeat 5 --block-bits 32 --output /tmp/C20_FLEET.json
python3 tools/validate_fleet.py --build-dir build/hip-release \
  --apache-root /path/to/private/apache-root --counts 2 --lifecycle-only \
  --reference-report /tmp/C20_FLEET.json --reference-device 0 \
  --output /tmp/C20_LIFECYCLE.json
```

## Lifecycle acceptance and remaining limits

The [calibrated live lifecycle run](baselines/C20_LIFECYCLE.json) passes with two
MI300X owners and the measured xpoint twelve-hour-target width:

- Three authenticated server pause/resume cycles retain both PIDs and consume no
  device failure budget (A16).
- A confirmed socket pause lasts over 65 seconds against a 60-second watchdog;
  the same owner resumes while its healthy peer continues making progress (A18).
- SIGSTOP makes one owner unresponsive. The bounded watchdog quarantines it;
  the healthy peer and an explicit machine sync continue. No hardware reset or
  actual driver failure is induced. Separate mocks cover an unreapable driver PID.
- Restart with only queue 1 and restricted visibility maps its original physical
  GPU from ordinal 1 to ordinal 0, retaining the UUID and unfinished block.
- A 128-byte host preparation budget rejects the BSGS queue while the xpoint queue
  completes both blocks. Only the failing queue is quarantined; journal checks pass.

A17 is addressed by retained process/table ownership and measured multi-grant
execution; A20's discovery path is corrected with HIP, mock and CUDA context
validation. The
frozen C15/C19 audit reports remain descriptions of their audited revisions.

Acceptance comprises **66 HIP/coordinator**, **47 CPU/coordinator**, **9 focused
debug** and **7 focused ASAN/UBSAN** gates, plus the 48-run fleet matrix and long
lifecycle run. [Raw test logs](baselines/C20_TEST_LOGS.tar.gz) retain the initial
failures and corrective reruns described above. No failures are waived. The
hardware scope is SPX/NPS1 on this host; CPX/QPX contracts remain covered by mocks.
These counts and measurements describe the original HIP acceptance. The
[H200 follow-up](C20_CUDA_VALIDATION.md) adds 64 CUDA-build tests, three GPU
memory/leak checks, 48 fleet runs and calibrated lifecycle recovery. Eight H200s
measure 6.32× xpoint and 4.60× BSGS finite-job throughput versus one H200.
Public ingress and cross-host coordinator/worker traffic remain outside both
localhost validations.
