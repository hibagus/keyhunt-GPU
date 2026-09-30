# C19 gfx942 arithmetic specializations

C19 is in progress. Each candidate retains the portable arithmetic and must pass
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
