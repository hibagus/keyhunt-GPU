# Implementation status

The [GPU redesign plan](GPU_REDESIGN_PLAN.md#10-commit-sized-implementation-sequence)
and [coordinator plan](COORDINATOR_SERVER_PLAN.md) define the roadmap. Each logical
change is committed separately. Analysis and validation evidence live under
`docs/`; planned behavior is not advertised as implemented.

| Milestone | Status | Evidence / remaining gate |
| --- | --- | --- |
| C01: CPU and environment baseline | Completed | [Analysis and reproduction](CPU_BASELINE.md); 38 characterization checks, raw build/hardware/timing artifacts |
| C02: source organization and CMake | Next | Preserve C01 behavior; legacy build needs GMP development headers on this host |
| C03–C23 | Planned | Acceptance gates remain in the redesign plan |

C01 records existing CPU boundary/stride defects rather than fixing them.
The new exact-range engine must not reuse those loops as proof of exhaustive
coverage. The optional legacy target could not be built because `gmp.h` is absent;
this limitation remains visible for C02 validation. No GPU search, checkpoint,
coordinator, or deployment has been implemented yet.
