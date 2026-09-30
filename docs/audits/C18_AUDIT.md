# C17–C18 H200 audit: merged CUDA acceptance

This follows the [C17–C19 MI300X audit](C19_AUDIT.md) using the user-authorized
H200 host over SSH. The search/arithmetic audit is frozen at **`b765880`**, the
same merged revision whose NVIDIA acceptance was previously unavailable.
The A20 context-isolation follow-up is separately frozen at **`fb57428`**.
Both were freshly built in isolated `/var/tmp` checkouts; the host's working
checkout and device configuration were left intact.

**All 55 tests in the fresh CUDA-build suite pass**, including 20 hardware tests
and 35 CPU/benchmark/coordinator tests, in **964.30 seconds**. Both CUDA arithmetic
variants and all 12 Compute Sanitizer runs pass as well. The fresh NVCC build,
search/recovery gates and live shared-code probes close the merged-revision
CUDA acceptance gap recorded by the previous audit. No new arithmetic, coverage
or memory defect was reproduced in these paths.

## Scope and environment

- Native **NVCC 13.3.73**, driver **610.57.04**, Release `sm_90` plus `compute_90`
  PTX, GCC 11.4, SQLite 3.51.3. No HIP runtime or CPU execution fallback is used
  by the CUDA search commands.
- The host exposes eight H200s. Correctness checks use visibility masks recorded
  with each invocation; the full regression run uses `CUDA_VISIBLE_DEVICES=7,6`
  to exercise multiple visible ordinals. Dedicated arithmetic, sanitizer and
  context tests also use other devices. Their timings are not performance data.
- Performance measurements use one H200 after the audit's functional tests and
  builds finish. The host is unreserved; power limits, clocks, compute modes and
  partitions are not changed. MIG and multi-GPU scaling are outside this audit.
- The coordinator is enabled, including the production HTTPS-worker build.
  Apache integration tests are not enabled on this host. This does not certify a
  deployed CUDA fleet or the other C20 scheduler/recovery requirements.
- `TMPDIR=/var/tmp` preserves the existing journal path guard. It avoids the
  H200 environment's managed `/tmp/.git` marker; no production guard is bypassed.

## C17 changes verified on CUDA

The previous audit already checked the C17 HIP tuning and retained paired
evidence. This follow-up checks the C17 changes that entered shared CUDA code:
candidate allocation, overflow recovery, dispatch accounting, exact ranges and
the backend-specific arithmetic/point-cache choices.

Both fresh probes pass five repetitions plus an excluded warm-up in each of
volatile and timed durability:

| Shared change | Native H200 result |
| --- | --- |
| A12: sparse-tail recovery after overflow | Capacity 1, maximum batch 256, four matches followed by a sparse tail: **28 launches**, one overflow, **4,095 verified scalars** and 4,351 attempted steps. All matches survive replay; both modes complete exact coverage. |
| A14: exact candidate capacity | Native executor and sanitizer tests preserve both opposite-sign X matches with a large requested buffer, still overflow with capacity 1, and retain complete BSGS target subsets with compact transfers. |
| A19: mixed durable BSGS groups | `m=17`, 8,192 giants, 32 targets split 31+1: both modes report **[1,8]**, with two launches of each group and **524,288 useful target giant steps**. Per-group counts and kernel time sum to the run totals. |

The probes reopen each durable journal and verify its completed interval and
stored results. Their process timings were collected while unrelated functional
tests ran on other GPUs and are used only as correctness evidence.

Native search tests also exercise dense results, both signs of the same X near
`n/2`, requested-capacity overflow, output guards, stale/foreign tickets, exact
tails, high-bit scalar ranges, full-public-key BSGS parity, collision handling,
and automatic/explicit group sizes. Overflowed attempts publish no coverage or
matches. Checkpoint tests cover process-exit recovery and pause/resume.

C17's HIP launch bounds and mixed-affine cache remain HIP-specific where the
CUDA measurements justified a different implementation. CUDA keeps its measured
mixed additions and direct-xpoint binary inversion. The C19 gfx942 helper does
not participate in this CUDA build.

## Arithmetic and memory checks

The default CUDA build and a separately compiled portable CUDA comparison each
pass **19,357 field cases** and **1,278 point cases** against the independent
Python/pinned-libsecp256k1 oracles. The field corpus includes 5,976 raw limb
carry/borrow cases; point tests cover aliases, infinity, opposite points,
doubling and mixed additions. The portable comparison disables the three CUDA
optimizations with `KEYHUNT_CUDA_PORTABLE_CARRY`,
`KEYHUNT_CUDA_PORTABLE_INVERSE` and `KEYHUNT_CUDA_PORTABLE_MIXED`; it preserves
the same C17 host-side buffer and recovery changes.

**Twelve Compute Sanitizer runs pass:** ten `memcheck` runs with full leak
checking, plus two `initcheck` search runs. They cover diagnostic/xpoint/table/
BSGS lifecycle, four failure-injection executables and both search benchmarks.
Every run reports zero errors; the memory checks report zero leaked allocations.
API-error reporting is disabled only for three lifecycle fixtures that
deliberately request invalid devices. Memory checking remains enabled there;
the other runs retain API-error reporting. These are memory/initialization gates,
not an exhaustive race or synchronization proof.
[NVIDIA sanitizer semantics](https://docs.nvidia.com/compute-sanitizer/ComputeSanitizer/index.html)

## A20: hardware reproduction and fix verification

The same native context observer is linked first to `b765880`'s production
self-test/backend libraries, then run against the fresh `fb57428` build. It calls
`cuInit`, verifies that no primary context is initially active, runs the worker's
four search variants on a nondefault ordinal, and queries context state without
retaining or selecting other contexts.

With `CUDA_VISIBLE_DEVICES=5,4`, the selected visible ordinal is **1**:

| Production revision | Observer result |
| --- | --- |
| `b765880` | Expected rejection: the worker self-test initialized an unrelated CUDA device. |
| `fb57428` | Pass: exactly **[1]** is active; xpoint direct/stepped and BSGS group 1/8 self-tests pass. |

The selected physical H200 is PCI `0000:9a:00.0`, UUID
`8793d79f350ec5010a954ce29deee8a0`. The observer source is unchanged between the
two runs; only the production implementation it links changes. This independently
confirms A20 and verifies its fix, including the nondefault-ordinal case that can
otherwise initialize device 0 during scope restoration.

**A20 is addressed at `fb57428`; it remains a valid finding for `b765880`.**
This probe does not simulate a driver hang, prohibited compute mode, unavailable
device or MIG partition, and it does not close the rest of C20's CUDA fleet gate.

## Fresh H200 performance

Timing uses physical GPU 7, PCI **`0000:dc:00.0`**, UUID
`ab387e304af7aaf1ece8575af15045ba`, 132 SMs, warp32, under
`CUDA_VISIBLE_DEVICES=7`. CUDA module/JIT/cache/launch-blocking environment
controls are unset. One excluded pair precedes **five alternating process
pairs** of portable and optimized CUDA builds from the same source. Each process
contributes a median of five checked warm samples.

| Warm workload | Optimized kernel rate | Optimized executor rate | Paired kernel speedup over portable CUDA |
| --- | ---: | ---: | ---: |
| Xpoint, one no-match target | **1.090 billion/s** | **1.061 billion/s** | **2.081×** |
| Xpoint, three boundary matches | 1.127 billion/s | 1.083 billion/s | 2.130× |
| Xpoint, 32 no-match targets | 0.484 billion/s | 0.478 billion/s | 1.709× |
| BSGS auto, one no-match target | **6.818 trillion/s** | **6.276 trillion/s** | **1.705×** |
| BSGS auto, three boundary matches | 6.334 trillion/s | 5.702 trillion/s | 1.737× |
| BSGS auto, 32 no-match targets | 1.003 trillion/s | 0.989 trillion/s | 1.738× |

Xpoint batches contain 1,048,576 scalars. BSGS uses **m=65,537 and 32,768 giants
per target**; the rates count scalar positions once across all targets, not full
scalar multiplications per second. Executor timing excludes process startup,
table preparation and journal work. The comparison measures the three accepted
CUDA arithmetic/point optimizations together; it does not isolate each one's
contribution. All five xpoint kernel ratios exceed 1.67×, and the auto-BSGS ratios
exceed 1.70×, across the tested cases.

Both builds include C17's compact buffers. Fresh one-target records confirm
**72 downloaded bytes for xpoint** and **80 for BSGS**, including the guarded
output and counters. This comparison should not be used as a speedup estimate
against a pre-C17 allocation policy.

A separate default-CUDA run searches **34,359,738,367 scalars** against one
known no-match target, using 32,768 stepped batches and the ten-second checkpoint
policy. One warm-up and five measured processes all complete with **four commits**
(three periodic and one final), exact persisted coverage and no matches.

| Whole-process measurement | Median of five runs |
| --- | ---: |
| Process elapsed time | **33.843 s** |
| Process scalar throughput | **1.015 billion/s** |
| Accumulated executor scalar throughput | **1.066 billion/s** |
| Preparation | 556.380 ms |
| Total time in four checkpoint calls | 2.577 ms |

All measured runs are retained. Process times range from **33.820 to 35.370 s**;
the slowest run reaches 0.971 billion scalars/s. Accumulated executor rates remain
within 1.0658–1.0662 billion/s, so the process outlier is not a comparable change
in measured executor throughput. Its cause was not isolated. Process timing
includes startup, execution, reporting and teardown; job creation and subsequent
journal inspection are separate. These finite runs do not establish multi-hour
or coordinated-fleet throughput, and the H200/MI300X reports are not a controlled
cross-vendor comparison.

## Exact executable resources and optimization priorities

`cuobjdump` inspected the freshly built production executable, with its binary
hash retained alongside the resource and SASS output:

| CUDA kernel | Registers/thread | Stack bytes/thread |
| --- | ---: | ---: |
| Stepped xpoint, ≤4 targets | 122 | 0 |
| Stepped xpoint, >4 targets | 110 | 768 |
| Direct xpoint | 94 | 64 |
| BSGS group 1 | 108 | 32 |
| BSGS group 8 | 92 | 1,024 |

These are compiler allocations, not measured occupancy or a measurement of
memory-traffic cost. Stack-heavy group-8 and large-target paths justify further
live-range/normalization-group experiments. Register caps or a higher nominal
occupancy are not sufficient acceptance criteria; compare complete checked
searches and preserve the distinct direct reference.
[NVIDIA Hopper resource guidance](https://docs.nvidia.com/cuda/hopper-tuning-guide/)

The H200 auto-group policy still chooses group 1 for the one-target,
32,768-giant case: `4 × 32 grouped blocks = 128`, just below the device's 132 SMs.
Explicit group 8 is faster in every one of the five optimized-build processes:
**1.054× median kernel speedup** (range 1.053–1.057×) and **1.046× executor
speedup** (range 1.042–1.054×). Its median executor time is 0.326295 ms versus
0.342166 ms for auto. This corroborates the workload-specific opportunity
already noted in C18; test a CUDA-specific threshold or use the existing override
for validated matching geometry. It does not justify forcing group 8 for smaller
grids or changing the HIP policy.

Persistent device owners remain important for short jobs because fresh CUDA
startup can dominate kernel time. C20's HIP evidence and A20's native fix are
separate from a full CUDA persistent-worker throughput assessment. Keep
whole-process and warm-executor figures distinct, and retain the direct,
overflow, oracle and recovery gates when tuning further.

## Retained evidence and reproduction

- [Fresh validation and build fingerprints](C18_AUDIT_VALIDATION.json).
- [Paired native CUDA measurements](C18_AUDIT_WARM.json),
  [periodic-checkpoint measurements](C18_AUDIT_CADENCE.json),
  [mixed-group checks](C18_AUDIT_GROUPS.json),
  [dense-prefix checks](C18_AUDIT_RECOVERY.json), and
  [independent evidence verification](C18_AUDIT_EVIDENCE.json).
- [Raw test, sanitizer, benchmark and SASS logs](C18_AUDIT_RAW_LOGS.tar.gz).
  Host executables, credentials, runtime journals and table caches are excluded.
- [CUDA comparison driver](../../tools/audit_c18.py). It reuses the frozen
  repository's checked warm benchmarks and records five alternating process
  pairs after an excluded pair. Five inner samples form one process median;
  they are not five independent process repetitions.

Build `b765880` with `KEYHUNT_ENABLE_CUDA=ON`, `CMAKE_CUDA_ARCHITECTURES=90` and
`KEYHUNT_ENABLE_COORDINATOR=ON`, using the compiler/dependency paths in the
validation manifest. Run the full CTest suite with `TMPDIR=/var/tmp` and at least
two visible devices. Build a second directory from the same checkout with the
three portable flags above for the arithmetic comparison. After tests finish:

```sh
CUDA_VISIBLE_DEVICES=7 TMPDIR=/var/tmp python3 tools/audit_c18.py \
  --source /path/to/frozen/b765880 --build /path/to/cuda-build \
  --portable /path/to/portable-cuda-build --output-dir /new/external/directory
```

Independent local verification checked all **443 archived files**, **168 command
stdout/stderr hash pairs**, **60 paired metric comparisons** and **99 matrix
distributions**. It reconstructed the CLI targets with the Python affine model,
checked saved coverage/match receipts and reopened-journal reports, and matched
129 source/build-file hashes per variant plus both benchmark-source hashes to
Git revision `b765880`.

The reports retain exact test/benchmark commands, source revisions, compiler
flags, GPU identity, visibility and raw-output hashes. The context observer and
its old/new link commands are also retained. Later implementation changes are
not silently included in these conclusions.
