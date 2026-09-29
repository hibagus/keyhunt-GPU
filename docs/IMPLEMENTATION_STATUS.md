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
| C06: independent arithmetic and search vectors | In progress | [Pinned oracle](ARITHMETIC_ORACLE.md) passes 1,082 independent checks; CPU primitive and search gates remain |
| C07–C23 | Planned | Acceptance gates remain in the redesign plan |

C01 records existing CPU boundary/stride defects rather than fixing them.
C05 supplies a separate exact host planning contract; it does not use those
loops as proof of exhaustive coverage or change their CLI behavior. C02 validated the optional legacy target with GMP development files extracted
under `/tmp`; no system package was installed. Sanitizer diagnostics and known
legacy defects remain documented follow-up work. No GPU search, checkpoint,
coordinator, or deployment has been implemented yet.
