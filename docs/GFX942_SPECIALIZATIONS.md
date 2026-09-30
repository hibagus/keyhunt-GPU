# C19 gfx942 arithmetic specializations

C19 is complete with opt-in compiler carry/borrow intrinsics. Each candidate retains
the portable arithmetic and must pass
independent oracles, exact search/recovery checks, disassembly inspection, and
five alternating measured process pairs after a warm-up pair. The initial gate
is a 5% median whole-kernel improvement outside noise, without a material default
workload regression. Assembly remains opt-in, per the [plan](GPU_REDESIGN_PLAN.md#assembly-policy).

## Rejected: limb multiply-accumulate

The first experiment replaces each schoolbook limb's `a*b + word + carry` with
`v_mad_u64_u32`, preserving the 33-bit `word+carry` addend. For 32-bit inputs its
maximum is `(2^32-1)^2 + 2*(2^32-1) = 2^64-1`; discarding the ISA carry-out is safe.
One instruction uses VGPR inputs/output and explicitly clobbers VCC. No suitable
Clang `__builtin_amdgcn_mad_u64_u32` was found: the installed Clang 23
`__has_builtin` probe returns false and the [Clang builtin reference](https://clang.llvm.org/docs/AMDGPUBuiltinReference.html)
contains no entry. The [MI300 ISA reference, V_MAD_U64_U32](https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-mi300-cdna3-instruction-set-architecture.pdf)
defines the multiply-add; [LLVM constraints](https://llvm.org/docs/LangRef.html#supported-constraint-code-list)
define register operands and clobbers. Sources consulted 2026-09-30.

The candidate passed 19,878 field cases (including 6,497 direct limb tests versus
Python integers) and 1,278 point cases. During development an overlapping rebuild
produced a stale probe with mismatched host/device operation enums; its first
field run failed. The completed rebuild and field rerun passed. Only completed,
frozen executables contributed timing evidence.

[Five paired trials](baselines/C19_MAD_REJECTED.json) against merge `02e2578` show
xpoint regressions and mostly small BSGS gains. This fails the acceptance gate;
the production implementation is removed. The [rejected patch](baselines/C19_MAD_REJECTED.patch)
applies to `02e2578` and includes the primitive, build option, and direct tests.
Its CMake option is experimental evidence, not an available feature.

Measurements used one MI300X SPX/NPS1 GPU (physical index 1, visible ordinal 0),
304 CUs, ROCm Core 10.0, HIP 7.15.26333, AMD Clang 23. The host was unreserved;
no partition, power, or clock settings were changed. Frozen executable/source
hashes, flags, device identity, every sample, and alternating order are in the
report. Results are finite synthetic workload measurements, not sustained or
multi-GPU performance claims.

## Exact-binary ISA evidence

[The capture tool](../tools/capture_hip_isa.py) extracts code objects from the
actual executable, disassembles them, and retains ELF kernel resource notes.
It hashes the executable, code objects, stdout and stderr, records every command,
and refuses existing or repository-local output directories. Extraction uses a
private copy because this LLVM version writes bundles alongside its input.
Compiler register/private-memory allocations are not measured occupancy.

```sh
python3 tools/capture_hip_isa.py \
  --binary /tmp/keyhunt-c19-before/keyhunt \
  --llvm-bin /opt/rocm/core-10.0/lib/llvm/bin \
  --output-dir /tmp/keyhunt-c19-isa-before
```

Validation: four embedded gfx942 objects were extracted from both the frozen
portable binary and carry-chain trial binary; all disassembled successfully.
A CPU-only binary correctly failed with no gfx942 objects, and an existing output
directory was rejected. Captures are separate from GPU timing trials.

## Retained: compiler carry/borrow chains

`KEYHUNT_GFX942_CARRY=ON` selects the small word-operation helpers in
[`carry.h`](../kernels/arch/amd/gfx942/carry.h). Clang's `__builtin_addc` and
`__builtin_subc` expose carry dependencies; the shared field code still owns
canonical reduction, subtraction correction, and aliasing. No handwritten ISA
is retained. Architecture, device-pass, compiler, and builtin guards preserve
portable code for host, CUDA, and other HIP targets. A configure-time static
library probe compiles both helpers without needing a GPU and fails clearly if
the requested compiler cannot build them.

```sh
# Reuse a configured HIP build with its existing local dependency paths.
cmake -S . -B build/hip-release -DKEYHUNT_GFX942_CARRY=ON
cmake --build build/hip-release --parallel 8
HIP_VISIBLE_DEVICES=1 ctest --test-dir build/hip-release --output-on-failure
# Return to the portable field operations:
cmake -S . -B build/hip-release -DKEYHUNT_GFX942_CARRY=OFF
cmake --build build/hip-release --parallel 8
```

The option defaults to **OFF**, including in the HIP preset. An explicit request
requires native HIP, AMD Clang, and at least one gfx942 code object. Mixed HIP
architecture builds retain portable code on other targets. The tested stack is
Clang 23 / HIP 7.15.26333 / gfx942 SPX/NPS1; remeasure after compiler upgrades.
No default dispatch, new search semantics, or persistent-format change is made.

[Five paired intrinsic trials](baselines/C19_CARRY.json) at 1,048,576 xpoint
scalars and 32,768 BSGS giants per target (`m=65,537`) give these median ratios.
Values above one mean less time for equivalent work. Warm executor time includes
transfers and host processing; these are not whole-process speedups.

| Workload | Kernel speedup | Warm executor speedup |
| --- | ---: | ---: |
| Xpoint, one outside target | 1.060× | 1.053× |
| Xpoint, three boundary matches | 1.051× | 1.042× |
| Xpoint, 32 outside targets | 1.010× | 1.008× |
| BSGS auto, one outside target | 1.024× | 1.022× |
| BSGS auto, three boundary matches | 1.023× | 1.025× |
| BSGS auto, 32 outside targets | 1.024× | 1.023× |

Only the first two workloads clear the 5% kernel gate. Other measurements show
small gains rather than a general 5% improvement. All emitted matches are
CPU-verified; the harness rejects changes to useful work and candidate counts.
The report retains five process pairs, each with five in-process samples,
reversed process order, an excluded warm-up pair, and variability statistics.

### Why compiler intrinsics instead of assembly

[The assembly trial](baselines/C19_CARRY_ASM.json) improves small-target xpoint
kernels by about 7%. Its [reconstructible patch](baselines/C19_CARRY_ASM.patch)
uses one statement per eight-limb chain: VGPR operands, VCC carry/borrow mask,
early-clobber outputs for limbs whose writes precede later input reads, and a
final 0/1 flag. It preserves exact aliases and changes no memory or EXEC state.
Both this variant and the intrinsic variant passed field/point oracles.

[Direct assembly-versus-intrinsic pairs](baselines/C19_CARRY_CHOICE.json) show
kernel ratios of 0.995–1.040×, below the 5% incremental threshold. Keep
the intrinsics because the small extra assembly gain does not justify owning
register constraints and clobbers. The raw-word tests remain useful for the
retained primitive: they compare exact 256-bit sums/differences and carry/borrow
flags against Python integers, including both aliases and every limb boundary.

### Disassembly and resource findings

The actual production executable's gfx942 code objects contain the compiler's
`v_addc_co_u32` / `v_subb_co_u32` chains. ELF notes show the small-target stepped
xpoint kernel using **148 VGPRs / 77 SGPRs**, versus **154 / 88** in the portable
baseline, with zero private memory in both. Direct and grouped-inversion xpoint
keep 248 VGPRs and 144/848 bytes of private memory; their SGPR counts decrease.
The BSGS group-1/group-8 allocations remain 248 VGPRs and 48/1,296 bytes of
private memory. No measured occupancy or spill-free claim follows from these
compiler allocations. The reports and disassembly retain the exact symbols.

The intrinsic helpers introduce no explicit registers or clobbers: LLVM owns the
VCC dependency and register allocation. The rejected assembly patch documents
those details for its own implementation. This is a carry/borrow specialization;
it changes neither multiplication/reduction algorithms nor field representation.

## Final correctness, recovery, and operating scope

The retained implementation is commit `99fa27a`; its directly relevant tests,
comments, build documentation, and raw evidence are committed together.

[Validation manifest](baselines/C19_VALIDATION.json): **33/33 CPU release,
59/59 HIP/coordinator regressions plus 2/2 additional HIP portable-fallback
oracles, 4/4 focused debug, and 4/4 focused address/undefined sanitizer checks**
passed. The new fallback target is registered only for the opt-in build, bringing
that configuration to 61 tests. It removes the optimization macro while running
the same independent vectors on the same real GPU.

Each portable-host, intrinsic-HIP, and portable-HIP probe passes **19,357 field
cases** (including 5,976 raw carry/borrow cases) and **1,278 point cases**.
The field corpus includes full-width carry/borrow flags, zero, values around
`p`, random operands, aliasing, inversion, and workgroup tails.
Search tests cover direct/stepped xpoint, all BSGS groups, wide offsets, tails,
all targets, candidate overflow, fault replay, checkpoint integrity, pause and
resume. Six compile/configuration checks pass: gfx942 specialization, gfx90a
and host fallback, CPU-only and non-gfx942 option rejection, and a mixed
`gfx90a;gfx942` configuration. No NVIDIA compiler or hardware gate was rerun.

[Paired CLI searches](baselines/C19_CLI.json) use 16 batches, 65,536-scalar xpoint
batches and 1,024 BSGS giants (`m=257`), with both volatile operation and a durable
commit after every batch. Independent expected matches, exact coverage receipts,
and journal audits agree for all five measured pairs and the excluded warm-up.
Warm executor ratios range from 1.004× to 1.084×; whole-process ratios range from
0.983× to 1.044×. Startup and storage dominate these short CLI jobs, so they do
not establish a general end-to-end speedup. They exercise explicit per-batch
persistence; they are not evidence of a ten-second periodic checkpoint interval.

[Direct xpoint pairs](baselines/C19_DIRECT.json) at 65,536 scalars improve median
kernel time by 1.054–1.057× across all three workloads. [Five measured pause
samples per geometry](baselines/C19_PAUSE.json), after an excluded startup sample,
preserve the exact saved interval union. The maximum observed pause-to-durable
status latency is below 1 ms. This is a finite sample on the tested device, not
a universal latency bound or a supervisor-level availability claim.

The final production executable is byte-for-byte identical to the measured
intrinsic candidate. [The evidence check](baselines/C19_EVIDENCE_CHECK.json)
recomputes all paired statistics, verifies command stdout/stderr hashes and ISA
artifacts, and checks final production source hashes. [Raw logs and ISA artifacts](baselines/C19_RAW_LOGS.tar.gz)
retain the temporary-directory prefixes referenced by the reports, frozen
metadata, compiler experiments, and the collection/dispatch-check scripts.
Executable snapshots, runtime journals, and table caches are excluded; rebuild
from the recorded sources and flags to repeat the measurements.

The option remains off by default. These results validate one primitive family
on one MI300X SPX/NPS1 timing device and the stated compiler stack. They do not
establish sustained rates, other partition modes, multi-GPU scaling, or the
resolution of the existing [C15 supervisor findings](audits/C15_AUDIT.md).
C20 remains separate work.
