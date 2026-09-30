# C17–C19 audit: HIP tuning, CUDA integration and gfx942 carry

Audited revision: **`b765880`**, 2026-09-30, from an isolated checkout with fresh
Release builds. Later storage and C20 working-tree changes are outside this
audit. Production source and implementation plans were not changed.

**61 HIP/coordinator tests, 43 CPU/coordinator tests and 18 focused default-HIP
tests passed.** No new arithmetic or coverage defect was reproduced. A12, A13,
A14 and A19 are addressed. One additional source finding, **A20**, concerns CUDA
worker self-tests initializing every visible device. Current-tree CUDA hardware
acceptance remains unverified here; the retained H200 results cover an earlier
revision.

## A20 — worker self-tests still discover every visible CUDA device

**P2, startup cost and failure isolation; established from source, not a new
NVIDIA hardware reproduction.** In the frozen revision:

- [self_test.cpp](../../src/coordinator/self_test.cpp):11 calls `discover_gpu()`
  before selecting the requested ordinal.
- [discovery.cu](../../src/backend/cuda/discovery.cu):51 enumerates every visible
  ordinal; `describe()` enters a `DeviceScope` and queries memory on each one.
- [coordinator_worker.py](../../tools/coordinator_worker.py):108 starts a fresh
  self-test process for each configured device. A failed self-test sets that
  selected device's failure count to three and aborts startup.

CUDA's `cudaSetDevice` initializes the device's primary runtime context and can
fail when another process owns an exclusive device or the device prohibits
execution. Thus a self-test for healthy device 0 can fail while visiting unrelated
device 1, before device 0's arithmetic/search tests run.
[NVIDIA runtime documentation](https://docs.nvidia.com/cuda/cuda-runtime-api/cuda_runtime_api/group__CUDART__DEVICE.html)

With M configured devices and G visible devices, the supervisor can perform
M×G device initialization visits across its fresh processes. C18's selected-device
optimization already avoids full discovery in search commands, but the self-test
does not use it. The retained [selection experiment](../baselines/C18_DEVICE_SELECTION.json)
measured 3,770 ms versus 756 ms for a small search before/after that optimization;
these are **search-command measurements, not measured self-test savings**.

Use the selected-device query for each worker self-test and obtain runtime/driver
versions without inspecting other devices. Validate with multiple visible GPUs,
a nonzero selected ordinal, and an unrelated device that cannot create a context.
Measure self-test startup separately from steady-state search. Restricting each
process's visibility to its assigned GPU also bounds this issue, but visible
ordinals must then be remapped consistently. This belongs with C20's ownership
and failure-isolation work.

## C17 finding closure

| Finding | Audit result |
| --- | --- |
| A12: permanent shrinkage after overflow | **Addressed.** Both volatile and durable paths use bounded batch recovery. Fresh dense-prefix runs with capacity 1 and batch limit 256 complete 4,095 scalars and four matches in **28 launches**, including one overflowing attempt; attempted work is 4,351. All five measured repetitions plus warm-up pass for both default and carry builds. |
| A13: missing xpoint launch bound | **Addressed.** HIP stepped kernels declare the actual 128-thread bound. Exact executable ELF notes show zero private segment for the small-target kernel in both builds. The original C17 paired launch-bound evidence also recomputes. |
| A14: allocation/transfer tied to requested capacity | **Addressed.** Xpoint caps the allocation at the minimum of requested capacity, maximum steps and twice the unique-X count. BSGS also uses its unique-public-key and 64-target limits. These bounds follow from the supported scalar interval and exact target uniqueness. Fresh one-target warm runs download **72 bytes for xpoint and 80 bytes for BSGS**, including counters and guard slots. Overflow/fault checks pass. |
| A19: only the final durable BSGS group reported | **Addressed.** Metrics version 2 retains per-group work, launches and kernel time. Fresh `m=17`, target-batch 31 runs report groups **[1,8]** in both modes. Each of two tiles executes 253,952 group-8 steps and 8,192 group-1 steps, totaling **524,288**, with complete coverage. |

The new mixed-group probe checks five repetitions plus warm-up for volatile and
timed durability, including reopened journal coverage and exact results. This
audit did not collect a new dispatch trace; it checks the new accounting against
the volatile dispatch receipts and the shared execution path. The earlier C16
audit retains the trace that established the original defect.

C17's mixed-coordinate changes remain selective: HIP group 1 uses mixed
addition; group 8 retains the measured general-addition path. The rejected
all-mixed experiment remains rejected. The short inversion chain, compact
buffers and batch recovery retain the direct/reference and overflow paths.

## Fresh MI300X measurements

One MI300X, **SPX/NPS1, 304 CUs, wave64**, PCI `0000:1b:00.0`, UUID
`30646366333934386337333264313231`, under `HIP_VISIBLE_DEVICES=0`.
Architecture `gfx942:sramecc+:xnack-`; HIP runtime/driver `71526333`.
The host was unreserved; no power, clock or partition settings were changed.
Audit GPU tests finished before timing started.

Default and carry executables were built from the same frozen source. The trial
uses one excluded process pair and **five alternating measured process pairs**.
Each process contributes the median of five checked warm samples. Those inner
samples are not counted as independent process pairs.

| Warm workload | Default HIP | Carry enabled | Unit |
| --- | ---: | ---: | --- |
| Xpoint, one no-match target | **2.766** | **2.872** | billion scalars/s |
| Xpoint, three boundary matches | 2.534 | 2.649 | billion scalars/s |
| Xpoint, 32 no-match targets | 1.233 | 1.266 | billion scalars/s |
| BSGS auto, one no-match target | **5.298** | **5.429** | trillion scalar positions/s |
| BSGS auto, three boundary matches | 3.446 | 3.518 | trillion scalar positions/s |
| BSGS auto, 32 no-match targets | 2.303 | 2.342 | trillion scalar positions/s |

These rates use accumulated executor wall time per warm batch, excluding startup,
table construction and journal work. Xpoint batches contain 1,048,576 scalars.
BSGS uses **m=65,537 and 32,768 giants per target**; scalar coverage is counted
once after all targets. BSGS coverage rates depend on this table/geometry and
are not full scalar multiplications per second or a general private-key rate.

Kernel-only one-target xpoint rates are **3.073 billion/s default** and
**3.225 billion/s carry-enabled**. Across paired processes, the carry kernel
speedup is 1.048× for one target and 1.049× for three targets; corresponding
executor speedups are 1.037× and 1.043×. The 32-target kernel ratios range from
0.995× to 1.085×, so that result is noisier. Auto-BSGS kernel median ratios are
1.021–1.025×.

This fresh small-target median is slightly below the tuning plan's 5% gate.
The retained C19 measurements showed 5.1–6.0%; the new result does not establish
a universal gain of that size. **Keep `KEYHUNT_GFX942_CARRY` opt-in.**

### Whole-process periodic-checkpoint run

The default HIP build searched **68,719,476,735 scalars** against one known
no-match target per process, using 65,536 stepped batches and a ten-second
checkpoint policy. One warm-up and all five measured runs completed exact
coverage with **three commits each**: two periodic checkpoints and the final
commit. Reopened journal coverage and results passed the harness, then the audit
checker verified the saved receipts and recalculated the rates.

| Metric | Median of five measured processes |
| --- | ---: |
| Whole-process elapsed time | **27.667 s** |
| Whole-process scalar throughput | **2.484 billion/s** |
| Accumulated executor scalar throughput | **2.711 billion/s** |
| Preparation | 515.209 ms |
| Sum of three checkpoint calls | 1.320 ms |

Process times range from 27.629 to 27.712 s. Process timing includes startup,
execution, reporting and teardown; initial job creation and later journal checks
are separate. The difference from warm-batch/kernel rates includes host and
process costs and cannot be attributed to checkpoint transaction time alone.
This is a repeated finite standalone run, not a twelve-hour or multi-GPU
coordinator throughput measurement.

## C19 arithmetic and compiler resources

The accepted specialization uses Clang's documented multiprecision carry/borrow
builtins, guarded by the opt-in flag, HIP device compilation and `gfx942`.
Host and other GPU architectures retain the portable path. The shared modular
correction and canonical representation remain in place.
[Clang builtin contract](https://clang.llvm.org/docs/LanguageExtensions.html#multiprecision-arithmetic-builtins)

Fresh carry and HIP-portable oracles each pass **19,357 field cases**, including
5,976 raw carry/borrow cases, and **1,278 point cases**, including aliases and
exceptional mixed additions. The host portable oracles also pass. The complete
carry-enabled suite covers search, exact tails, failure injection, recovery,
pause/resume and local coordinator integration. The default build additionally
passes 18 focused arithmetic, executor, discovery, table and checkpoint checks.

Actual embedded code objects were extracted and disassembled from both newly
built executables. Selected ELF-note allocations are:

| HIP kernel | Default VGPR / SGPR | Carry VGPR / SGPR | Private bytes/lane, both |
| --- | ---: | ---: | ---: |
| Stepped xpoint, ≤4 targets | 154 / 88 | 148 / 77 | **0** |
| Stepped xpoint, >4 targets | 248 / 88 | 248 / 75 | 848 |
| Direct xpoint | 248 / 86 | 248 / 76 | 144 |
| BSGS group 1 | 248 / 104 | 248 / 104 | 48 |
| BSGS group 8 | 248 / 106 | 248 / 106 | **1,296** |

These are compiler allocations, not measured occupancy. The large-target and
group-8 kernels still justify experiments to reduce live intermediates or compare
smaller normalization groups. Retain mixed-addition and grouping comparisons:
reducing arithmetic counts already regressed the recorded group-8 experiment.
The rejected gfx942 multiply-add and handwritten carry assembly patches should
remain outside production unless new whole-kernel evidence changes the decision.

## CUDA scope and remaining acceptance

Source review covers the native CUDA adapter, device restoration, PTX carry
constraints, arithmetic dispatch, cached-point representation and shared launch,
buffer and checkpoint paths. The default NVIDIA arithmetic policies remain
distinct where C18 measured a benefit, including binary inversion for direct
xpoint. C19's gfx942 helper is excluded from CUDA compilation.

This machine has **neither an NVIDIA device nor `nvcc`**. The retained C18 report
records 47 distinct CUDA-build tests, 32 CPU tests and Compute Sanitizer checks
on H200 hardware. This audit recomputes **15 large-batch comparisons**, plus
**354 distributions from 90 measured durability runs and 18 warm-ups**. Those
durability JSON records retain command hashes, but their raw command-log archive
is not present here; the audit does not claim to have revalidated their raw
journals or rerun NVIDIA hardware tests.

The H200 validation names `d483dc2`, with runtime sources at `cc32365`; the
durability report also names `cc32365`. C17 subsequently changed shared candidate
allocation, xpoint batch recovery and checkpoint metrics, and merged at
`02e2578`. **A clean NVCC build and focused H200 parity/recovery tests on the
merged revision remain an acceptance gap.** HIP passes cannot close it. Include
the production worker self-test and unrelated-device failure case from A20 in
that run. Do not directly rank the H200 and MI300X figures: their revisions,
timing environments and experimental designs differ.

## Evidence and next priorities

- [Fresh build/test validation](C19_AUDIT_VALIDATION.json),
  [independent evidence checks](C19_AUDIT_EVIDENCE.json), and
  [audit checker](../../tools/audit_c19.py).
- [Fresh paired warm measurements](C19_AUDIT_WARM.json),
  [dense-prefix regression](C19_AUDIT_RECOVERY.json),
  [mixed-group probe](C19_AUDIT_GROUPS.json), and
  [periodic-checkpoint measurements](C19_AUDIT_CADENCE.json).
- [Raw logs, oracle outputs and exact-binary ISA](C19_AUDIT_RAW_LOGS.tar.gz).
  The archive excludes host executables, credentials, runtime journals and table caches.
- The checker independently verifies all **21 C17 manifest file hashes**,
  **15 retained C17/C19 paired reports**, **2,048 command stdout/stderr pairs**
  and **792 paired metric comparisons**. It reconstructs synthetic targets with
  the independent Python affine model and checks retained CLI coverage/results.
  C17's mixed-group matrix adds 40 distributions. It also checks 16 retained ISA
  code objects and their command logs, plus the expected CPU-negative capture.
- **131 source/test hashes** match the C19 validation snapshot. The sole changed
  snapshot Markdown file is `kernels/README.md`; this is a documentation change,
  not an executable-source mismatch.

**A16, A17 and A18 remain open at the audited revision.** The C18 supervisor
changes add backend selection but do not repair the earlier pause/quarantine
logic or per-grant process startup. See the [C15 audit](C15_AUDIT.md). Their
reproductions were not repeated here. Persistent per-device owners and table
reuse remain the strongest next application-level performance work; standalone
kernel speed does not measure the supervisor's utilization.

Reproduce evidence checks without running a GPU:

```sh
python3 tools/audit_c19.py --source /path/to/frozen/b765880 \
  --report /tmp/C19_AUDIT_EVIDENCE.json
```

For fresh warm trials, create default and `KEYHUNT_GFX942_CARRY=ON` builds from
that same source, snapshot both with `tools/benchmark_gpu_pair.py snapshot`, then
run its `compare --suite warm --repeats 5` command under `HIP_VISIBLE_DEVICES=0`.
The JSON reports retain exact commands, geometry, compiler information, visibility
and binary fingerprints. Pass their external `report.json` paths to the audit
checker with `--pair` or `--matrix` to check fresh raw logs too.
