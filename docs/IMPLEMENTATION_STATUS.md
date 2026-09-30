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
| C11: complete HIP BSGS range search | Next | Exact residual/mapping, giant-step tails, all targets, candidate replay and CPU/oracle parity |
| C12–C23 | Planned | Acceptance gates remain in the redesign plan |

C01 records the original CPU boundary/stride defects. C06 fixes modular/point
arithmetic and the tested BSGS start miss; tail overrun and stride defects remain.
C05 supplies a separate exact host planning contract. C02 validated the optional
legacy target with GMP development files extracted
under `/tmp`; no system package was installed. Sanitizer diagnostics and known
legacy defects remain documented follow-up work. C09 uses the exact planner for
HIP xpoint search; checkpointing, coordination and deployment remain unimplemented.
