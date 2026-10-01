# Implementation status

The [GPU redesign plan](GPU_REDESIGN_PLAN.md#10-commit-sized-implementation-sequence)
and [coordinator plan](COORDINATOR_SERVER_PLAN.md) define the roadmap. Each logical
change is committed separately. Analysis and validation evidence live under
`docs/`; planned behavior is not advertised as implemented.

| Milestone | Status | Evidence / remaining gate |
| --- | --- | --- |
| C01: CPU and environment baseline | Completed | [Analysis and reproduction](CPU_BASELINE.md); 38 characterization checks, raw build/hardware/timing artifacts |
| C02: source organization and CMake | Completed | [Migration evidence](BUILD_MIGRATION.md) and [build guide](BUILD.md); CPU release/debug, isolated GMP legacy parity, daemon loopback and install checks |
| C03: README and usage guides | Completed | [Redesign notes](README_REDESIGN.md); current commands, mode reference, preserved historical examples |
| C04: core extraction | Completed | [Extraction notes](CORE_EXTRACTION.md); shared configuration, owned target loaders, CPU verification; release/debug regressions pass |
| C05: exact ranges and work units | Completed | [Exact range contract and evidence](EXACT_RANGES.md); checked 256-bit ranges, immutable blocks and bounded direct-scan work/batches; exhaustive and wide oracle tests pass |
| C06: independent arithmetic and search vectors | Completed | [Oracle and correction evidence](ARITHMETIC_ORACLE.md); pinned libsecp256k1, field/point/search cases, arithmetic fixes; release/debug and focused sanitizers pass |
| C07: HIP discovery and execution backend | Completed | [HIP contract and findings](HIP_BACKEND.md); gfx942 launches, explicit partition metadata gaps, bounded asynchronous ownership, 20 failure-injection boundaries; [original validation](baselines/C07_VALIDATION.json) and [CPX/QPX/SPX compatibility](HIP_BACKEND.md#cpx-qpx-and-spx-compatibility) |
| C08: portable GPU field and point arithmetic | Completed | [Arithmetic contracts, findings and evidence](GPU_ARITHMETIC.md); 13,381 field and 1,278 point cases, independent oracles, alias/zero/tail checks, eight SPX devices, measured radix comparison |
| C09: bounded HIP xpoint searches | Completed | [Search contract and evidence](HIP_XPOINT.md); full-X targets, exact high-bit ranges, CPU verification, bounded candidate overflow/replay, measured point stepping |
| C10: versioned GPU BSGS table preparation | Completed | [Table format and acceptance](BSGS_TABLES.md); 4,765 pinned-oracle baby entries, checksums/rehashed-corruption rejection, exact collision lists, memory budgets, CPU/HIP filter parity and eight SPX devices |
| C11: complete HIP BSGS range search | Completed | [Search mapping and evidence](HIP_BSGS.md); exhaustive tiny/seeded high-offset cases, signed points/infinity, exact tails, all-target replay, eight SPX devices and measured grouping |
| C12: sparse coverage and transactional assignments | Completed | [Storage contracts, commands and measurements](STORAGE.md); private versioned SQLite, exact sparse rank selection, merged coverage, fenced ownership, concurrent/idempotent claims and quarantined backup/restore; [acceptance evidence](baselines/C12_VALIDATION.json) |
| C13: checkpoint verified progress and replay incomplete work | Completed | [Checkpoint contracts, commands and findings](CHECKPOINTS.md); canonical input binding, atomic verified matches/coverage, fourteen process-exit faults, real HIP restart, migration/corruption gates and a corrected BSGS type collision; [acceptance evidence](baselines/C13_VALIDATION.json) |
| C14: graceful pause, resume and inspection | Completed | [Controls, operations and measurements](PAUSE_RESUME.md); graceful signals, private local commands, bounded drain, resume fencing, online snapshots and real HIP restart across 2/1/3 visible devices; [acceptance evidence](baselines/C14_VALIDATION.json) |
| C15: authenticated project-scoped coordination | Completed (localhost scope) | [Implementation and operations](COORDINATOR.md), [findings and validation](COORDINATOR_VALIDATION.md); required mTLS, registry/roles, atomic machine sync, durable outbox, offline fences, supervised HIP execution and reconciled restore. User-selected localhost gate replaces second-host/public ingress; those remain deferred. |
| C16: reproducible GPU profiling and benchmarks | Completed | [Methodology, findings and raw evidence](GPU_PROFILING.md); exact oracle-checked coverage, repeated volatile/durable samples, hardware/build metadata, real periodic commits, separate ROCm traces/counters and compiler resources; 32 CPU and 58 HIP/coordinator tests pass |
| C17: measured HIP tuning | Completed | [Changes, rejected experiment and paired evidence](HIP_TUNING.md); xpoint kernels 1.94–2.33× and BSGS kernels 1.31–1.72× faster on the measured MI300X workloads; exact overflow recovery, compact transfers, complete grouping metrics, pause checks; 33 CPU / 59 HIP / 8 debug / 6 sanitizer tests pass |
| C18: native CUDA | Completed | [CUDA build, tuning and acceptance](CUDA_BACKEND.md); 47 CUDA-build and 32 CPU tests, eight H200s, exact xpoint/BSGS parity, checkpoint recovery, 12 sanitizer runs and measured PTX/mixed-coordinate optimizations; [validation evidence](baselines/C18_VALIDATION.json) |
| C19: gfx942 arithmetic specializations | Completed (opt-in) | [Carry/borrow intrinsics, rejected experiments and ISA evidence](GFX942_SPECIALIZATIONS.md); small-target stepped xpoint kernels 1.051–1.060× faster, portable fallback; 33 CPU / 61 HIP / 4 debug / 4 sanitizer checks pass |
| C20: concurrent GPU scheduling | Completed (HIP and CUDA / localhost) | [Ownership and operations](MULTI_GPU.md); persistent owners, balancing, calibrated widths and bounded recovery. HIP: 66 HIP / 47 CPU / 9 debug / 7 sanitizer gates and [manifest](baselines/C20_VALIDATION.json). [H200 acceptance](C20_CUDA_VALIDATION.md): 64 CUDA-build tests, three GPU memory/leak checks; each backend has 48 validated 1/2/4/8-GPU runs and calibrated lifecycle recovery. |
| C21: validated GPU build and operations guides | Completed | [Findings and acceptance](C21_VALIDATION.md); tested build/mode matrices, executable HIP/CUDA quickstarts, CPU CI preparation, localhost worker operations, recovery/performance limits and checked documentation links |
| C22: offline assignment export and reconciliation | Completed (trusted courier / localhost) | [Workflow and recovery](OFFLINE_ASSIGNMENTS.md), [acceptance](C22_VALIDATION.md); schema-v7 transfer receipts, exclusive files, duplicate/fencing/deadline checks, exact unions, bounded reconciliation and disconnected HIP/CUDA xpoint/BSGS |
| C23: further GPU mode coverage | Partial: named families, scalar strides/reverse/orbits, GLV and reverse/both-ends/dance BSGS tiles and reverse/both-ends/dance minikeys complete | [Bitcoin mainnet P2PKH/HASH160 acceptance](C23_VALIDATION.md): HIP/CUDA algorithms, exact overflow/recovery, encoding-bound results, capability fencing and online/offline owners. [Ethereum acceptance](C23_ETHEREUM_VALIDATION.md) adds Keccak, ERC-55, durable recovery and worker parity. [Vanity acceptance](C23_VANITY_VALIDATION.md) adds exact P2PKH prefixes, overlapping relations and recovery. [Minikey acceptance](C23_MINIKEYS_VALIDATION.md) adds exact 22/30-character ordinal coverage, both encodings and recovery. [Stride contracts](C23_STRIDES.md) add exact positive scalar progressions, candidate-index recovery and compatible worker gates. [Reverse acceptance](C23_REVERSE_VALIDATION.md) adds both kernel orders, unit/nonunit strides, durable recovery and compatible online/offline workers. [GLV acceptance](C23_GLV_VALIDATION.md) adds opt-in multiplication without changing coverage. [Orbit acceptance](C23_ORBITS_VALIDATION.md) adds explicit six-member candidate expansion. [Reverse BSGS tiles](C23_BSGS_REVERSE_VALIDATION.md) preserve actual scalar receipts across direction changes and worker recovery. [Both-ends BSGS](C23_BSGS_BOTH_ENDS_VALIDATION.md) alternates low/high tiles with adaptive work accounting. [Exact dance BSGS](C23_BSGS_DANCE_VALIDATION.md) adds a bounded low/high/middle cycle with fixed midpoint and exact recovery. [Reverse minikey acceptance](C23_MINIKEYS_REVERSE_VALIDATION.md) adds exact descending ordinal execution with compatible checkpoints and workers. [Both-ends minikey acceptance](C23_MINIKEYS_BOTH_ENDS_VALIDATION.md) adds alternating endpoint batches, same-end overflow replay and exact adaptive/restart coverage. [Dance minikey acceptance](C23_MINIKEYS_DANCE_VALIDATION.md) adds fixed-midpoint selection, bounded reservations and recovery in every order. Additional random traversal semantics still require separate parity gates. |

C01 records the original CPU boundary/stride defects. C06 fixes modular/point
arithmetic and the tested BSGS start miss; legacy tail overrun and stride defects remain. The separate C23 native stride interface has an exact candidate-index contract.
C05 supplies a separate exact host planning contract. C02 validated the optional
legacy target with GMP development files extracted
under `/tmp`; no system package was installed. Sanitizer diagnostics and known
legacy defects remain documented follow-up work. C09 uses the exact planner for
HIP xpoint search; C11 adds a separate exact BSGS tile mapping. C12 supplies the
local storage foundation; C13 adds verified durable GPU checkpoint execution through
separate commands. C13 also fixes the legacy/canonical BSGS target type-name collision
found by debug and sanitizer creation tests. C14 adds durable local pause/resume,
graceful signals and live control inspection. C15 adds authenticated coordination
and isolated localhost deployment with separate worker journals. Public ingress
and a physical second host remain deferred by user decision. C20 adds
simultaneous multi-GPU execution with independent device processes. CPX/QPX/SPX contracts remain
supported, with current hardware validation on SPX/NPS1 and no partition changes.

C16 adds comparable standalone execution/durability metrics and opt-in measurement
tools without changing search kernels or persistent schemas. C15 supervisor
findings in the [frozen audit](audits/C15_AUDIT.md) are addressed by C20.

C17 retains portable arithmetic while improving measured kernel throughput,
candidate memory/transfer bounds and sparse-tail overflow recovery. Each
optimization is committed separately with comments and paired evidence. The
[acceptance scope and limits](HIP_TUNING.md#pause-validation-and-limits) distinguish
kernel/executor gains from startup-sensitive process rates and preserve the
supervisor findings subsequently addressed by C20. C17 itself makes no
handwritten ISA or multi-GPU speedup claim.

C19 adds opt-in gfx942 compiler carry/borrow intrinsics with a configure-time
capability check and a real-HIP portable arithmetic fallback gate. The
[validation manifest](baselines/C19_VALIDATION.json) and
[paired evidence](GFX942_SPECIALIZATIONS.md) separate kernel improvements from
startup-sensitive CLI timings. Multiplication ISA was rejected for regressions;
explicit carry assembly was not retained because its incremental gain over
intrinsics stayed below the acceptance threshold.

C20 completes concurrent machine execution on the validated HIP and CUDA hosts.
Eight MI300X GPUs measure 6.04× xpoint and 4.75× BSGS finite-job throughput;
eight H200s measure 6.32× and 4.60× respectively, each relative to one GPU on its
own host. Both retain exact coverage and verified results. A16–A18 and A20 have
live recovery/context evidence; the H200 report preserves the rejected startup
setting experiment and leaves existing kernel choices unchanged.
