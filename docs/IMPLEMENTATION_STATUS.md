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
| C04: core extraction | In progress | [Extraction notes](CORE_EXTRACTION.md); configuration separated, target loading and verification next |
| C05–C23 | Planned | Acceptance gates remain in the redesign plan |

C01 records existing CPU boundary/stride defects rather than fixing them.
The new exact-range engine must not reuse those loops as proof of exhaustive
coverage. C02 validated the optional legacy target with GMP development files extracted
under `/tmp`; no system package was installed. Sanitizer diagnostics and known
legacy defects remain documented follow-up work. No GPU search, checkpoint,
coordinator, or deployment has been implemented yet.
