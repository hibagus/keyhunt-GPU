# C20–C21 audit: concurrent GPU execution and published guides

Scope: C21 completion revision `f7242a0db5ff843768e3c9d4360332f35556748c`,
including the C20 persistent-worker, dispatch, recovery and calibration changes.
The audit used isolated local and SSH checkouts. Concurrent C22 development is
outside this review.

**The normal execution and documented-command gates pass. One new P2 recovery
finding remains: A21, an owner that publishes `stopped` but never exits escapes
the automatic watchdog. A22 identifies a measured BSGS tuning opportunity using
existing CLI settings.** No incorrect GPU result or overlapping accepted
coverage was observed. A16, A17 and A18 are independently closed for the tested
HIP/CUDA paths; the selected-device context test also confirms A20's fix.

## A21 — keep an exit deadline after the terminal event

Priority: **P2, reproduced supervisor recovery defect**.

At the frozen revision, [Device.stalled](../../tools/coordinator_worker.py)
lines 106–110 excludes `stopped` alongside legitimate indefinite `idle` and
`paused` states. The main loop only completes a slot after `child.poll()` reports
process exit (lines 278–304). Consequently, a live child that reports `stopped`
and then wedges is neither complete nor subject to automatic drain/quarantine.
A finite `--once` supervisor can wait indefinitely.

This is reachable after the production worker's terminal notifications:
[run_device](../../src/coordinator/device_worker.cpp) lines 197–200 emits
`control:stopped` and `exit:drained` **before** its local `Prepared` object is
destroyed. The retained GPU executors still run their destructors, including
stream synchronization and device/host allocation release. The terminal event
is therefore not proof that process teardown has finished. NVIDIA also documents
that ordinary-allocation `cudaFree` may synchronize; this supports retaining a
teardown deadline, not a claim that this hardware actually hung. [CUDA memory API](https://docs.nvidia.com/cuda/cuda-runtime-api/cuda_runtime_api/group__CUDART__MEMORY.html)

The [reproduction](C21_AUDIT_TERMINAL_STALL.json) runs the unchanged supervisor
with a synthetic child that reports `ready`, `control:stopped` and `exit:drained`,
then remains alive. After **65.07 seconds** with `--stall-seconds 60`, the supervisor
still has a live stopped PID and an empty failure map. A deterministic clock
probe also remains exempt after 1,000,000 seconds. Manual supervisor termination
cleans up the fixture. This models a teardown hang; it does not inject a GPU
driver fault or demonstrate lost coverage.

Required correction: start a bounded terminal-exit deadline and keep observing
the PID until it is reaped. Preserve the existing shared drain/kill limits and
owner-lock protection for unkillable processes. A terminal event must not be an
indefinite watchdog exemption. Test both a prompt normal exit and a child that
publishes terminal events but remains alive, including healthy-peer progress.
Resetting the deadline on entry to teardown avoids treating time spent paused
as time spent shutting down.

## A22 — use the launch budget for small BSGS target sets

Priority: **P2, measured performance opportunity**, with supported CLI overrides.

The supervised defaults in [run_device](../../src/coordinator/device_worker.cpp)
lines 92–94 use 16,384 giants and a target-batch limit of 64. With only two
canonical targets, actual work is **32,768 target-giant steps per launch**, though
the executor permits 1,048,576. The small launch also makes automatic dispatch
choose group 1 for the full tiles on these GPUs. Increasing the giant batch to
524,288 while setting the target batch to 2 keeps the same one-million-step
ceiling and lets full tiles use the existing group-8 kernel.

This audit compared those two supported settings, using an isolated variant of
the fleet harness. Inputs, binary, UUID, durability policy and total work stay
identical: `m=257`, targets G and −G, two blocks totaling 8,589,934,585 scalars.
Each host has an excluded warm-up pair and **five measured pairs**, alternating
which variant runs first. Every run repeats the exact coverage, match, journal
and explicit-upload checks. No production kernel or supervisor code is patched.

| GPU | Whole-process paired speedup | Warmed-grant paired speedup | Kernel paired speedup |
| --- | ---: | ---: | ---: |
| MI300X | 1.70× | 7.20× | 12.05× |
| H200 | Inconclusive (0.50–2.85× pair range) | 3.83× | 4.34× |

MI300X process medians are **1.959 → 1.152 seconds**. H200 process samples
vary from 2.081–5.088 seconds for baseline and 1.787–4.182 seconds for candidate;
its 1.08× median paired process ratio is not sufficient evidence of a repeatable
end-to-end gain. H200 warmed-grant medians are **420.158 → 110.014 ms**,
and kernel totals are **643.002 → 148.244 ms**. All samples are retained.
The source of the H200 process-overhead variation was not identified.

Speedups are medians of five within-pair ratios, not ratios of pooled launches.
Kernel time sums both blocks; warmed-grant time measures the second block in the
same persistent owner; process time includes startup/self-test and shutdown.
This is a **single-GPU, two-target, small-table result**. It cannot be multiplied
by eight to claim an unmeasured tuned fleet rate, or applied to another target
population/table without measurement.

The [paired data and independent recomputation](C21_AUDIT_GEOMETRY.json) retain
all samples and spreads. Supplemental volatile searches check exact matches
against the independent Python curve oracle at a large tile boundary, a short
range tail, and near the curve order. Both launch settings pass all three cases
on each backend. Batch output confirms default group 1 versus candidate groups
8 for full tiles and 1 for the short tail. Those checks separate a faster launch
from accidentally skipping work. The raw archive contains both live drivers;
[the checker](../../tools/audit_c21_geometry.py) recomputes work, matches and paired
statistics without running a GPU.

For this measured two-target workload, the supported supervisor options are:

```sh
# Append to the otherwise unchanged supervisor invocation:
--target-batch 2 --giant-batch 524288
```

Before changing automatic defaults, derive effective target-batch width from the
canonical target count, preserve the total-step ceiling and explicit overrides,
and rerun target-group tails, dense-match replay and pause-latency gates. Larger
kernels change the time until a control boundary. Record launch geometry in new
reference measurements and recalibrate widths for new immutable jobs; do not
silently resize an existing assignment. The measured improvement combines
larger batches and automatic group selection, so this experiment does not
attribute the gain to either change alone.

## Earlier findings and ownership review

| Finding | Evidence at the audited revision |
| --- | --- |
| A16, server pauses consumed the GPU failure budget | Three real authenticated pause/resume cycles preserve both PIDs and the failure budget on each backend |
| A17, per-grant process/table setup | Each device retains one executor across multiple grants; all fresh fleet completions report one executor setup per owner |
| A18, socket pauses triggered the stall watchdog | Confirmed local pauses exceed the 60-second watchdog while a healthy peer keeps progressing |
| A20, workers initialized unrelated CUDA devices | The production self-test's fresh-process primary-context gate passes with all eight H200s visible |

Dispatch uses a transaction and a unique project/job/block binding. A faster
owner can claim another queue's unstarted same-job block, while active work stays
bound across restart. Per-queue locks protect device identity; shared fleet
locking plus a separate OFD byte-range lock excludes duplicate execution of a
block. Independent file descriptions retain distinct locks, matching the
[Linux OFD lock contract](https://man7.org/linux/man-pages/man2/F_OFD_SETLK.2const.html).
Fresh concurrent-checkpoint and dispatch tests exercise exclusion, retained
ownership, UUID replacement, crash replay and healthy progress beside a stopped
owner. The sanitizer checks cover the changed host ownership paths.

The live lifecycle checks additionally quarantine a SIGSTOP-stalled running
owner, continue its peer and explicit sync, restart queue 1 as visible ordinal 0,
and isolate a BSGS preparation-budget failure from its xpoint peer. These results
close the earlier pause defects, but do not cover A21's later teardown stage.

## Fresh validation

| Check | Fresh result |
| --- | --- |
| Portable HIP / coordinator / HTTPS | 64/64 pass, no skips |
| Native CUDA / coordinator / HTTPS | 64/64 pass, no skips |
| CPU-only build | 34/34 pass |
| Changed host ownership paths, ASan/UBSan | 7/7 pass, leak detection and halt-on-error enabled |
| Documented quickstarts | CPU preparation, MI300X ordinal 0 and H200 ordinal 7 pass |
| Localhost operations | HIP and CUDA readiness, execution, explicit upload and acknowledgment pass |
| Default fleet matrices | 48 runs / 360 blocks on each backend; all selected GPUs contribute |
| Lifecycle | Both backends pass server/local pause, running-owner stall, remapping and budget isolation |
| BSGS geometry | Five measured pairs plus a warm-up pair per backend; six additional oracle edge runs per backend |
| Documentation and installation | 48 frozen Markdown files, workflow parse, CPU README examples, installed supervisor/calibration commands pass |

The [validation manifest](C21_AUDIT_VALIDATION.json) records frozen source hashes,
build settings, commands, binaries, artifact checksums and load observations.
[Raw logs](C21_AUDIT_RAW_LOGS.tar.gz) retain CTest output, detailed test logs,
oracle/search reports, build logs and the operations reproduction script.
[Examples](C21_AUDIT_EXAMPLES.json), [operations](C21_AUDIT_OPERATIONS.json), and
[lifecycle reports](C21_AUDIT_LIFECYCLE.json) retain their output separately.

The CPU build requires no GPU SDK. HIP uses the documented portable default
(`KEYHUNT_GFX942_CARRY=OFF`); the retained C20 HIP baseline enabled C19 carry
intrinsics. An initial audit configure argument misspelled the opt-in switch and
was explicitly reported unused by CMake. The actual cache and generated build
remain portable; none of the fresh measurements are labeled carry-enabled.
CUDA uses the native `sm_90` defaults, CUDA 13.3.73 and driver 610.57.04. Both hosts
expose eight physical GPUs; MI300X is SPX/NPS1 and H200 has MIG disabled.

## Fresh finite-job performance

Each cell has one excluded warm-up and five measured runs. Each job contains
`2 × GPU-count` nominal `2^32`-scalar blocks, with a seven-scalar final tail. Xpoint has
one full-X target. BSGS uses `m=257` and two full public keys, G and −G. Timings
include supervisor/worker startup, fresh self-tests, durable execution and
shutdown; setup of assignments and later upload/inspection are excluded.

**MI300X / portable HIP**

| GPUs | Xpoint G scalars/s | Relative to one | BSGS G effective scalars/s | Relative to one |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.917 | 1.00× | 4.326 | 1.00× |
| 2 | 3.605 | 1.88× | 7.732 | 1.79× |
| 4 | 6.498 | 3.39× | 13.583 | 3.14× |
| 8 | 10.872 | 5.67× | 21.206 | 4.90× |

**H200 / native CUDA**

| GPUs | Xpoint G scalars/s | Relative to one | BSGS G effective scalars/s | Relative to one |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.719 | 1.00× | 1.914 | 1.00× |
| 2 | 1.376 | 1.91× | 3.482 | 1.82× |
| 4 | 2.558 | 3.56× | 5.812 | 3.04× |
| 8 | 4.503 | 6.26× | 8.993 | 4.70× |


Rates are billions per second. **BSGS measures effective scalar coverage**, not
individual scalar multiplications or the same work as xpoint. The
[MI300X fleet](C21_AUDIT_HIP_FLEET.json) and
[H200 fleet](C21_AUDIT_CUDA_FLEET.json) preserve all timings and grants.
Each backend passes 48 runs and 360 block completions. Every selected device
contributes. Fresh eight-MI300X BSGS trials redistribute unstarted work: owners
complete one to four blocks each while preserving the exact 16-block union.
This is live balancing evidence; do not substitute the older dataset's exactly
two completions per owner. The harness checks an
exact contiguous block union, a CPU-verified scalar-1 match, local journal
integrity, pending outbox before explicit sync, and server acknowledgment after
upload. The independent evidence checker recomputes grant geometry, scalar and
target-giant counts, setup reuse, medians and relative scaling.

Keep the full ranges in the raw statistics: the MI300X eight-device BSGS samples
span **3.032–4.031 seconds**, despite a 3.241-second median. No sample was removed.
These hosts were not reserved, and process observations cannot exclude every
brief external workload. The HIP and CUDA datasets are separate measurements,
not a controlled cross-vendor comparison or twelve-hour endurance result.

## Performance actions supported by the evidence

1. **Amortize startup with continuing owners and suitably sized immutable jobs.**
   In the retained H200 BSGS data, the one-GPU job takes 4.479 seconds while the
   sum of that owner's two grant bodies is about 0.876 seconds. At eight GPUs,
   the job takes 7.793 seconds and the slowest owner's grant-body sum is about
   0.894 seconds. Most finite-job elapsed time is outside the grant bodies.
   The equivalent MI300X residuals are about 0.985 and 2.290 seconds. These
   residuals include scheduling gaps, initialization and teardown; they do not
   identify a single API bottleneck. Keep the per-process self-test, reuse the
   prepared executor, and benchmark longer work in an already-running owner
   before deciding that short-job scaling is a kernel limitation.
2. **Measure startup phases and sustained scaling separately.** Record runtime
   selection, self-test, table preparation/upload, dispatch wait and teardown
   spans, then compare long same-work runs with normal durability enabled.
   C20 already removes per-grant executor allocation. Its rejected CUDA preload
   experiment supplies no evidence to change runtime environment defaults.
3. **Add representative table sizes and target populations to fleet trials.**
   `m=257` validates coordination and recovery, but cannot establish HBM-sized
   table behavior, NUMA placement or aggregate host-copy pressure. Each owner
   currently prepares its own immutable host table. Measure peak memory and
   upload/steady execution costs before attempting shared read-only host input
   storage or CPU affinity changes. Retain per-device memory checks and exact
   identity validation in any such experiment.

A22 above supplies measured speedups from existing launch options; the actions
in this list remain measurement and implementation proposals. Retain C19 as opt-in; this audit does not
change CUDA arithmetic/group policies or the GPU clock, power or partition mode.

## Evidence and C21 documentation review

[The evidence checker](../../tools/audit_c21.py) verifies 513 retained source
fingerprints, 18 C21 publication fingerprints, 50 archived log members, and all
listed artifact hashes. It recomputes both original 48-run fleet datasets and
the four published calibration recommendations with rational arithmetic. The
original figures are consistent: eight MI300X measured 11.944 G xpoint/s and
21.048 G BSGS effective/s; eight H200 measured 4.507 and 8.818 respectively.
Fresh HIP uses a different carry setting, so those two HIP runs are not a paired
regression test. [Recomputed evidence](C21_AUDIT_EVIDENCE.json) records both sets.

One provenance qualification: the original HIP manifest names `5d1e3c9`, but its
`tools/validate_fleet.py` hash includes the calibration/remapping follow-up later
committed as `3dfdd76`. That individual hash matches the retained follow-up file;
the other 255 source fingerprints match the named commit. Reproduction must
honor both the source revision and the recorded harness hash. CUDA has no such
source-fingerprint difference.

The C21 executable harness reads the Bash commands from the quickstart itself.
Fresh CPU preparation and both hardware runs check fixed coverage, scalar-1
results and completed-grant retries with zero new batches. The operations runs
verify local completion remains unacknowledged until explicit upload. Build
instructions, mode boundaries, hexadecimal half-open ranges, queue mapping,
signals, checkpoint state, installed supervisor/calibration commands and the
SQLite dependency setup agree with the frozen implementation. All 48 frozen
Markdown files pass local link, anchor and fence checks. The CPU commands pass
locally; this does not claim a new GitHub-hosted CI run.

The fleet JSON retains per-grant work/timing rather than every server response.
The independent checker can reconstruct those counts and rates; the fresh live
harness additionally repeats the match, integrity and acknowledgment assertions.
No credential contents or private worker state are included in the audit files.

## Reproduction and limits

```sh
python3 tools/audit_c21.py --source FROZEN_C21_CHECKOUT --output evidence.json
python3 tools/audit_c21.py --source FROZEN_C21_CHECKOUT \
  --terminal-probe --output terminal-stall.json
python3 FROZEN_C21_CHECKOUT/tools/validate_fleet.py --build-dir BUILD \
  --apache-root APACHE_ROOT --backend hip --counts 1,2,4,8 \
  --repeat 5 --block-bits 32 --output hip-fleet.json
python3 FROZEN_C21_CHECKOUT/tools/validate_fleet.py --build-dir BUILD \
  --apache-root APACHE_ROOT --backend hip --counts 2 --lifecycle-only \
  --reference-report hip-fleet.json --reference-device 0 --output lifecycle.json
```

Use `cuda` with a separate native build on H200. Keep `TMPDIR` outside Git
checkouts (`/var/tmp` on that host); exact environment and commands are retained.
Pass additional `--fleet FILE` arguments to the evidence checker for fresh fleet
reports. The terminal probe deliberately waits 65 seconds and cleans up its
synthetic process group. A successful reproduction means the defect is present.

Twelve-hour widths remain predictions from five warmed single-device grants.
SIGSTOP and a 128-byte preparation budget model failures; genuine driver hangs,
large-table fleet pressure, CPX/QPX search scaling, MIG and physical cross-host
coordinator traffic remain outside this acceptance. Existing C18/C20 CUDA memory
checks are retained evidence; this audit's new sanitizer run covers the host
ownership paths. C22–C23 and legacy CPU engine defects are not certified here.
