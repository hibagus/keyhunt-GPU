# C14–C15 audit and refreshed MI300X measurements

Audited revision: `33acd335cc1bc92753418f47d2b0bbaabbd2b69b`, **2026-09-30**.
An archived checkout isolates this review from concurrent C16 implementation.
Production code was not changed by this audit.

**57 HIP tests and 41 CPU tests passed**, including the real localhost mTLS
worker, recovery, pause/resume and fault-injection tests. Additional audit probes
found two availability defects outside that coverage: legitimate coordinator
pauses and a local socket pause can quarantine a healthy device queue until
explicit retry.

Warm one-target throughput remains **1.47 billion xpoint scalars/s** and
**3.21 trillion BSGS scalar-range positions/s with `m=65,537`**. These are
different work metrics, not interchangeable public-key evaluation rates.

## Scope and evidence

- One MI300X, SPX/NPS1, 304 CUs, wave64, `gfx942:sramecc+:xnack-`; runtime
  ordinal 0 under `HIP_VISIBLE_DEVICES=0`, PCI `0000:1b:00.0`.
- Release builds with explicit `gfx942`; HIP runtime/driver `71526333`.
  Hardware identity matches the C13 audit. No power, clock or partition changes.
- Independent CPU and HIP builds ran their complete suites: 41 tests in 65.89 s
  and 57 tests in 357.22 s. Audit performance runs started after both suites.
  This was an unreserved host, not a controlled thermal or power experiment.
- All coordination probes used temporary state, synthetic targets and private
  loopback Apache/mTLS processes. They did not use public ingress or another host.
- [Validation records and build/source fingerprints](C15_AUDIT_VALIDATION.json),
  [raw performance and lifecycle evidence](C15_MI300X_AUDIT_PROBES.json), and
  [reproduction script](../../tools/audit_c15.py).

The core/kernel/backend diff from C13 is empty. This audit does not establish
multi-GPU scaling, CPX/QPX rates, large-table BSGS performance, NVIDIA execution,
or long-duration thermal stability. The C15 sanitizer results published by the
implementation were not independently rerun here.

## Warm executor performance

Three processes provide five retained samples each, **15 per workload and selected
kernel**. Xpoint uses the stepped kernel with 1,048,576 scalars per batch. BSGS
uses automatic grouping, 32,768 giants per target and `m=65,537`, covering a tile
of 2,147,516,416 scalar positions. BSGS warm-up samples (`sample=-1`) are excluded.

Rates divide scalar width by median executor wall time, including seed creation,
transfers and CPU candidate verification. Startup, preparation and checkpoints
are excluded. Every benchmark checks expected match counts and scalars.

| Workload | Xpoint kernel ms | Xpoint wall ms | Xpoint scalars/s | BSGS kernel ms | BSGS wall ms | BSGS scalar-range positions/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| One target, no match | 0.669161 | 0.714518 | **1.468 billion** | 0.629270 | 0.668904 | **3.211 trillion** |
| Three boundary targets, three matches | 0.709251 | 0.765489 | **1.370 billion** | 0.765660 | 0.818601 | **2.623 trillion** |
| 32 targets, no match | 1.872093 | 1.917642 | **547 million** | 1.231880 | 1.274325 | **1.685 trillion** |

BSGS credits the scalar tile once after all targets complete. Its coverage rate
depends on table size and target count; it is not 3.21 trillion curve evaluations
per second. These results are consistent with the earlier one-target rates.
The 32-target BSGS result varies despite unchanged kernels; these short samples
do not establish a code regression or its cause.

## A16 — normal coordinator pauses quarantine a healthy device

Priority: **P2, reproduced availability defect**. Frozen source references:
[supervisor](../../tools/coordinator_worker.py), lines 120–124 and 148–153;
[assignment authorization](../../src/storage/journal.cpp), lines 103–114.

The assignment gate correctly refuses submissions when the coordinator pauses a
job. The HIP child exits with `keyhunt: coordinator paused this job`. The
supervisor counts every nonzero child exit as a device failure, including this
expected control outcome, and skips queues whose count reaches three.

The audit started a real HIP worker against the private HTTPS coordinator and
performed three server pause/unpause cycles, using explicit manual syncs to
deliver each control. Its recorded failure count progressed **1 → 2 → 3**.
After the final unpause, native `keyhunt-worker next --device 0` returned a valid,
executable grant, while the supervisor retained `failures:{"0":3}` and would
not dispatch it. Each child log contained the coordinator-pause message, not a
GPU failure. Journal integrity checks passed. The audit then terminated its own
otherwise idle supervisor; the saved quarantine survives process restart.

Use a structured child termination reason to distinguish coordinator pause,
expiry/fencing and authorization refusal from execution faults. Keep dispatch
blocked while the control requires it, and allow valid resumed work without
spending the hardware failure budget. Preserve explicit quarantine for actual
execution errors. Add an integration test covering repeated authenticated
pause/unpause with a live executor and persisted supervisor state.

## A17 — short grants spend most of their time starting processes

Priority: **P2, measured lifecycle cost**. Frozen
[supervisor](../../tools/coordinator_worker.py), lines 108–116 and 158–177,
runs a fresh device self-test at startup and starts a new `keyhunt checkpoint run`
process for each block. Each block reconstructs its HIP owner. The supervisor
also rewrites and fsyncs the same target file, and observes completion on its
100 ms loop. These costs are outside the warm executor figures.

The audit measured three separate two-block xpoint jobs at each size. Targets
were known to be outside the range. Setup and the initial HTTPS assignment sync
were excluded; fresh self-tests, all child startup, durable execution, supervisor
polling and cleanup were included. Each worker completed both blocks locally,
retained the outbox until upload, passed a journal check, and produced exactly
two finished server blocks after an explicit sync.

| Nominal width per block | Blocks | Median supervisor process time | Effective xpoint scalars/s |
| --- | ---: | ---: | ---: |
| 1,048,576 | 2 | **2.763 s** | **759 thousand** |
| 4,294,967,296 | 2 | **8.953 s** | **959 million** |

For independent manifests, repeats add zero, one or two scalar positions to each
nominal width; rates use the exact counts. Observed times were 2.491–2.797 s and
8.895–9.015 s. These are finite-job rates including the startup self-test, not
the sustainable speed of an already running supervisor. The experiment does
not isolate the cost of each individual startup or fsync.

Amortize initialization across grants with a retained device executor, target
binding and, for BSGS, device table. Revalidate grant identity and journal fences
at each handoff; retain the fresh startup self-test and existing ownership rules.
Use completion notification or adaptive polling to avoid a fixed polling delay
between short grants. Until that lifecycle work lands, choose block sizes from
measured sustained runtime rather than from the kernel batch size.

## A18 — a socket-paused child is mistaken for a stalled GPU

Priority: **P2, reproduced control integration defect**. Frozen
[supervisor](../../tools/coordinator_worker.py), lines 89–102 and 180–190.
The supervisor suppresses its watchdog only when its own `paused` flag is set
by `SIGUSR1`. A direct `keyhunt checkpoint pause --state-dir ...` command reaches
the child's C14 control socket and never updates that supervisor flag.

The audit requested a socket pause from a real supervised HIP owner and waited
for `durably_paused:true`. With the supported `--stall-seconds 60` setting, the
supervisor subsequently marked `failures:{"0":3}` and terminated that correctly
paused child. The child drained cleanly with `complete:false`, yet the supervisor
exited with its quarantine error. The journal integrity check passed; this is
an availability failure, not observed false coverage or lost committed state.
The production default uses the same code with a 300-second threshold.

A paired control paused the owner through **signals sent to the supervisor**,
waited 62 seconds, then resumed. It recorded `paused` followed by `running`, kept
an empty failure map, and stopped cleanly when the audit requested cleanup.
Until integration is fixed, use the documented supervisor signal path for
supervised work; the standalone C14 socket control remains valid for a standalone
owner. The problem is combining that control with the supervisor's watchdog.

Give the supervisor authoritative child activity information, or route all local
control through a single owner. A durably paused executor must not consume a
progress-stall budget. Keep an independent readiness/progress signal so ordinary
log growth is not the sole evidence that execution is healthy. Add a regression
covering a socket pause longer than the configured watchdog and a subsequent
resume, alongside the already successful supervisor-signal case.

## Earlier findings and optimization order

1. **A13 remains open:** xpoint still lacks the actual 128-thread launch bound.
   The [C11 isolated experiment](C11_AUDIT.md#a13--declare-the-actual-128-thread-xpoint-launch-bound)
   measured 1.48–1.51× kernel throughput and approximately **2.13 billion
   one-target xpoint scalars/s** using executor wall time. That is historical
   experimental evidence, not the current C15 rate or a rerun of the patch.
   Validate spills, occupancy and all correctness gates when integrating it.
2. **A12 remains open:** overflow permanently reduces the xpoint batch limit,
   including in the checkpoint runner. Keep the bounded replay fix on the list;
   dense-prefix/sparse-tail work can otherwise remain in tiny batches.
3. **A14 remains open:** candidate buffers are cleared and downloaded at their
   configured capacity. Derive safe workload bounds and measure reduced transfer
   volume while preserving overflow and guard checks.
4. **A15 remains open in this frozen revision:** checkpoint summaries expose
   counts and transaction time but omit aggregate executor timing. Add timing
   through the coordinated path so kernel, initialization, validation, I/O and
   queue starvation can be distinguished. Concurrent C16 work is outside scope.
5. **A08 now has a live integration example:** each measured worker consumed its
   two saved blocks and correctly declined an early scheduled sync. With at most
   one active block plus one spare and a two-hour contact interval, a queue of
   short blocks cannot sustain GPU utilization. Preserve that explicit network
   policy; estimate queued runtime, show expected idle time, and size the immutable
   block grid for the intended active-computation duration. The plan's 12-hour
   target must be derived from the relevant GPU, mode and target/table workload.

Reproduce after other GPU jobs finish:

```sh
HIP_VISIBLE_DEVICES=0 python3 tools/audit_c15.py \
  --source /path/to/archived/33acd33 \
  --build /path/to/isolated/hip-build \
  --apache-root /path/to/apache-package-root \
  --revision 33acd335cc1bc92753418f47d2b0bbaabbd2b69b \
  --report /tmp/C15_MI300X_AUDIT_PROBES.json
```
