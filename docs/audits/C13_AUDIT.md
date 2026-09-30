# C12–C13 audit and MI300X performance figures

Audited revision: `2e4513a`, **2026-09-30**. Builds and source review used an
archived checkout; concurrent C14 working-tree changes are outside this report.

**No new correctness failure was found in the audited C12/C13 paths.** Independent
runs passed 44 HIP tests, 29 CPU-only tests, eight focused ASan/UBSan tests, and
five historical missed-target fixtures through durable HIP execution. Completed
fixture retries performed zero additional GPU batches. The earlier xpoint
performance findings remain open; the permanent overflow batch reduction also
exists in the new checkpoint runner.

The current implementation reaches **1.47 billion searched scalars/s for one-target
xpoint** on one MI300X in the warm executor benchmark. BSGS with `m=65,537`
reaches **3.21 trillion scalar-range positions/s** for one target. Those metrics
describe different amounts of GPU arithmetic and must not be treated as
interchangeable public-key evaluation rates.

## Hardware, build and evidence

- One AMD Instinct MI300X, SPX/NPS1, 304 CUs, wave64; `gfx942:sramecc+:xnack-`.
  PCI `0000:1b:00.0`, runtime ordinal 0 under `HIP_VISIBLE_DEVICES=0`.
- HIP runtime/driver `71526333`, Release build, explicit `gfx942` compilation.
  SQLite 3.51.3 from the installed standalone ROCm sysdeps library; state files
  were on local ext4 under `/tmp`, outside the source checkout.
- No clock, power, partition or memory-partition changes. This was an unreserved
  host. Other audit validation jobs finished before the performance runs began.
- [Validation evidence](C13_AUDIT_VALIDATION.json): exact revision/source hashes,
  binary fingerprints, build options, test output and real HIP restart results.
- [Raw MI300X and regression evidence](C13_MI300X_AUDIT_PROBES.json) and
  [reproduction runner](../../tools/audit_c13.py).

This is one-device evidence. Eight-device throughput, CPX/QPX performance, NVIDIA
execution, C14 pause controls and power-loss recovery were not tested here.

## Current warm MI300X throughput

Each benchmark process warms its executors and validates expected matches/counts.
Three processes contribute five measured samples each: **15 samples per workload
and selected kernel**. Xpoint uses the current stepped kernel and 1,048,576
scalars per batch. BSGS uses automatic grouping, `m=65,537`, 32,768 giants per
target, and a scalar tile of 2,147,516,416 positions.

Rates below divide scalar width by median **executor wall time**, including seed
construction, transfers and CPU candidate verification. They exclude process
startup, executor/table preparation and durable checkpoint work.

| Workload | Xpoint kernel ms | Xpoint wall ms | Xpoint scalars/s | BSGS kernel ms | BSGS wall ms | BSGS scalar-range coverage/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| One target, no matches | 0.669761 | 0.714512 | **1.468 billion** | 0.629791 | 0.668713 | **3.211 trillion** |
| Three boundary targets, three matches | 0.707607 | 0.761993 | **1.376 billion** | 0.763494 | 0.814908 | **2.635 trillion** |
| 32 targets, no matches | 1.875661 | 1.922866 | **545 million** | 1.172663 | 1.214047 | **1.769 trillion** |

BSGS credits each scalar tile once after all targets complete. It does not multiply
coverage by the target count. Its corresponding target giant-step rates are
approximately 49.0 million/s, 120.6 million/s and 863.7 million/s. Its coverage
rate depends on `m`, the target count and the table workload; these small-table
numbers are not measurements with HBM-sized tables.

The old [C11 launch-bound experiment](C11_AUDIT.md#a13--declare-the-actual-128-thread-xpoint-launch-bound)
remains separate: it measured approximately **2.13 billion xpoint scalars/s** for
one target using executor wall time. The patch is still absent from the audited
C13 kernels. Its earlier 1.48–1.51× kernel throughput improvement is experimental
evidence, not the current production rate or a newly rerun C13 variant comparison.

## Longer CLI runs with durable checkpoints

The earlier C13 timing workload had only sixteen small batches, making process
startup dominate. This audit compared 64 and 4,096 full batches, using a fixed
high-bit scalar start and one independently derived no-match target. It retained
three samples per mode/cadence/size after one short warm-up per path, rotating
variant order and alternating size order: **36 measured execution processes**.

The long xpoint job covers 4,294,967,296 scalars. The long BSGS job covers
8,796,227,239,936 scalar positions using `m=65,537`, 32,768 giants per tile, one
target and automatic grouping. Every durable run passed the journal integrity
audit and had an empty stored result set afterward.

| Mode | Cadence | Median execution-process ms | Process scalar-range rate/s | Checkpoints | Median total checkpoint transaction ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Xpoint | Volatile | 3,720.897 | 1.154 billion | 0 | 0 |
| Xpoint | Default ten seconds | 3,575.437 | **1.201 billion** | 1 | 0.587 |
| Xpoint | Every batch | 4,265.212 | 1.007 billion | 4,096 | 533.493 |
| BSGS | Volatile | 3,781.507 | 2.326 trillion | 0 | 0 |
| BSGS | Default ten seconds | 3,836.739 | **2.293 trillion** | 1 | 0.613 |
| BSGS | Every batch | 4,706.817 | 1.869 trillion | 4,096 | 549.514 |

These process times include startup, target/table loading, HIP initialization,
execution, output and cleanup. They exclude creating the job/claim, initially
building the table, and the post-run integrity/result queries. The timed runs
finish before ten seconds, so their single checkpoint is the final commit. They
do not measure repeated ten-second checkpoint cycles over a long-running job.

The host variability is material: xpoint volatile times span 3,411–3,732 ms and
timed times span 3,479–3,992 ms. The lower timed median does **not** establish that
checkpointing makes the GPU faster. Output volume also differs: volatile commands
emit every batch, whereas timed checkpoint commands emit commit acknowledgments.
These are actual command timings, not a controlled isolation of SQL overhead.

What is directly measured is the transaction cost: 4,096 commits consume about
0.53–0.55 seconds, versus roughly 0.6 ms for the default final commit. Keep the
existing batched cadence unless a smaller replay window is required. Preserve
immediate match commits and atomic results/coverage. Reducing SQLite durability
would change the recovery guarantee; FULL WAL synchronization is part of that
contract. [SQLite synchronous documentation](https://www.sqlite.org/pragma.html#pragma_synchronous)

The raw file also retains rates estimated by subtracting the 64-batch process
time from its corresponding 4,096-batch time. Subtraction amplifies startup and
host noise, so those estimates are not used as the headline device figures.

## Findings and next optimization work

### A12 remains open in durable xpoint execution

Priority: **P2, reproduced performance defect**. In the frozen
[checkpoint.cpp](../../src/storage/checkpoint.cpp), lines 142–159, an overflow
reduces `limit` for all subsequent work without recovery. The C06 fixture
`[0x200,0x600)` with six targets and capacity 1 completes correctly but requires
**1,025 batches for 1,024 scalars**: one overflowing attempt and 1,024 singleton
retries. The aligned order-tail case has the same count. Matches survive and
finished retries do no GPU work, so this remains a throughput defect, not an
observed durable-coverage failure.

Apply the bounded interval splitting or batch recovery solution to both
`xpoint_command.cpp` and the checkpoint owner. Require dense-prefix/sparse-tail,
near-order, crash/restart and geometry-change checks. Preserve the zero-credit
rule for overflowed attempts and the ordering of durable coverage advancement.

### A13 and A14 remain open

The current xpoint declarations still lack the 128-thread launch bound already
used for BSGS. The prior isolated patch is the strongest measured next kernel
optimization. Revalidate it on the implementation revision selected for merging.

Both executors also continue clearing/downloading candidate slots according to
requested capacity. The prior sparse large-capacity overhead finding remains
applicable; this follow-up did not repeat that capacity sweep. Use the proven
maximum possible candidate count to bound allocations/transfers while retaining
overflow semantics and guard validation.

### A15 — Expose GPU and host stages in durable performance summaries

Priority: **P2, instrumentation improvement for C16**. The checkpoint summary at
[checkpoint_command.cpp](../../src/backend/checkpoint_command.cpp), lines 137–141,
contains counts and `checkpoint_ms`, but omits kernel, seed, transfer, CPU
verification, fencing and total execution wall times. `checkpoint_ms` measures
only the commit call at `checkpoint.cpp:107–110`; it cannot explain the rest of
the throughput difference. Short process benchmarks and subtraction estimates
cannot reliably supply those missing stages, as the overlapping samples above
demonstrate.

Accumulate the existing executor timing fields in the checkpoint owner and add
host validation/commit/total wall measurements, keeping GPU device identity and
accepted scalar counts with the result. Emit aggregate metrics without requiring
per-batch output. Keep process startup/table preparation separate from warm work.

[journal.cpp](../../src/storage/journal.cpp), lines 255–256, performs a fresh
read transaction and repeated SQL preparation for each bounded batch. Measure
this stage before optimizing it; prepared-statement reuse is a plausible next
experiment. Retain the current assignment, epoch, expiry and executor-generation
checks. This audit does not attribute the whole timing difference to fencing.

## Correctness and persistence review

Source review covered sparse free-space selection, canonical input binding,
assignment/executor fencing, result verification, atomic match/coverage commits,
all-target BSGS replay, schema migration and sealed backups. The C13 BSGS type
rename correctly separates the canonical target class from the legacy loader;
the focused sanitizer run includes BSGS creation and the new storage paths.

| Independent validation | Result |
| --- | --- |
| HIP Release, GPU 0 | **44/44**; 373.90 seconds |
| CPU Release | **29/29**; 36.04 seconds |
| CPU Debug with ASan/UBSan, storage/checkpoint and BSGS contract selector | **8/8** |
| Extra historical fixtures through durable HIP | **5/5**, including all previously missed targets |
| Finished-job retry of each extra fixture | **5/5**, zero GPU batches |

The existing suites exercise sixteen concurrent allocation writers, two allocation
process exits around COMMIT, fourteen checkpoint exits across seven boundaries in
both modes, real HIP kill/restart with changed geometry, lost stdout after COMMIT,
corrupted receipts/results/bindings, and quarantined backups. These test process
recovery, not storage-device power-loss behavior. No new blocking finding emerged
from this scope; the report does not certify concurrent C14 changes.

## Reproduction

Build an archived `2e4513a` checkout with Release, `BUILD_TESTING=ON`,
`KEYHUNT_ENABLE_HIP=ON`, and `CMAKE_HIP_ARCHITECTURES=gfx942`. SQLite paths and
the separate CPU/sanitizer build options are preserved in the validation JSON.

```sh
HIP_VISIBLE_DEVICES=0 ctest --test-dir /path/to/hip-build --output-on-failure --parallel 1

HIP_VISIBLE_DEVICES=0 python3 tools/audit_c13.py \
  --build /path/to/hip-build \
  --repeats 3 --long-batches 4096 \
  --report /tmp/c13-mi300x-audit.json
```

Original isolated builds and logs: `/tmp/keyhunt-c13-audit-3w2_500h/`. Only audit
tools/documents/evidence were added to the shared repository by this follow-up;
the implementation under review was not modified.
