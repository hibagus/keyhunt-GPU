# C17 measured HIP tuning

C17 starts from the [C16 baseline](GPU_PROFILING.md). Each retained optimization
gets an independent commit, arithmetic/search/recovery checks, compiler evidence
where relevant, and alternating equivalent baseline/candidate trials. Raw
measurements distinguish kernel, warm executor, and fresh-process rates. BSGS
scalar coverage and target giant steps remain separate units.

## Paired trials and frozen builds

Build the HIP targets before freezing each version. Snapshots retain exact
executables, source and binary hashes, compile commands, device/topology and tool
metadata; later edits cannot replace the baseline silently. The warm benchmark
uses known outside-range targets and CPU-verifies every emitted match. It can
select stepped xpoint alone and vary candidate capacity. End-to-end trials reuse
C16's independent oracle, exact receipts and post-run durable journal audit.

```sh
cmake --build build/hip-release -j 8
HIP_VISIBLE_DEVICES=1 python3 tools/benchmark_gpu_pair.py snapshot \
  --build-dir build/hip-release --output-dir /tmp/c17-before
# Apply one candidate change and rebuild, then freeze it under /tmp/c17-after.
HIP_VISIBLE_DEVICES=1 python3 tools/benchmark_gpu_pair.py compare \
  --baseline /tmp/c17-before --candidate /tmp/c17-after \
  --output-dir /tmp/c17-pair --label candidate --suite both --repeats 5
```

Each comparison has an excluded warm-up pair followed by at least five measured
pairs, reversing process order every pair. Warm executors contribute five samples
per workload per process; comparisons use the **process median**, so those samples
are not mistaken for five independent process pairs. Reports retain raw samples,
paired ratios, median, spread and MAD. Exact source/input/device checks reject
incompatible work; failed processes cannot contribute an accepted result. All
runs are opt-in, use synthetic inputs and external state, and change no hardware
settings. CLI defaults compare volatile and ten-second checkpoint execution.

The initial acceptance threshold is a 5% median workload improvement outside
observed noise; justified exceptions must identify another measured benefit and
show no material default-workload regression. Always preserve exact coverage,
all overflow matches, recovery and bounded pause behavior. The available host is
unreserved; disclose other load and avoid simultaneous profiling or correctness
jobs on the measured GPU. Compiler resource reports are estimates, not measured
occupancy.

## Initial candidates

1. A13: supply the actual 128-thread xpoint launch bound and inspect spills.
2. Replace unnecessary general point-add work when the cached operand is affine.
3. Reduce field inversion work with a verified addition chain; evaluate squaring
   only if complete kernels improve.
4. A14: size candidate output using exact mathematical limits while retaining
   overflow, guard and counter validation.
5. A12: recover large xpoint batches after overflow without repeated work or
   failure to terminate on fully dense inputs.

Architecture-specific assembly remains an option when whole-kernel measurements
justify it; retain a portable reference. C19's broader ISA delivery gate remains
separate from C17. This document will record accepted and rejected experiments
and the final validation scope as measurements complete.

## Accepted: xpoint launch bounds (A13)

Both xpoint entry points now declare the 128-thread maximum used by every host
launch. On gfx942, five alternating measured pairs at 1,048,576 scalars produced
1.525× / 1.528× / 1.591× median kernel speedups for no-match one-target,
boundary-three-target and no-match 32-target workloads. Warm executor speedups
were 1.465× / 1.476× / 1.562×; every individual pair improved.
[Raw paired measurements](baselines/C17_LAUNCH.json) retain distributions, samples,
exact commands and frozen-build identities.

The [compiler remarks](baselines/C17_LAUNCH_RESOURCES.log) report zero spilled
VGPRs for all xpoint kernels. Small-target stepping has zero scratch; grouped
inversion still needs 784 bytes/lane of private scratch, so zero spills does not
mean zero private memory. Compiler occupancy estimates are not measured occupancy.
Five focused HIP tests passed: executor tails/ownership/overflow, injected
failures, both CLI kernels and checkpoint pause. The existing C16 report provides
the pre-change compiler baseline. Measurements used physical GPU 1 (visible
ordinal 0); an independent C16 audit used GPU 0 on the same unreserved host.

## Accepted: measured mixed point addition

The portable mixed-add primitive specializes a Jacobian plus affine operand,
preserves infinity/equal/opposite points and in-place aliasing, and removes four
field multiplies and one square from a normal addition. Xpoint's cached powers
now use 68-byte affine values instead of 96-byte Jacobian values; no coordinate
normalization is needed because the CPU seeds are already affine. Single-giant
BSGS also uses mixed addition. Its grouped kernel retains the general formula.

[Five measured pairs](baselines/C17_MIXED.json) against the launch-bound build show
xpoint kernel speedups of 1.281× / 1.292× / 1.080× and warm executor speedups of
1.259× / 1.256× / 1.077×. Single-giant BSGS improves 1.089–1.180× at kernel level.
The unchanged grouped BSGS workloads remain within about 1% of baseline.
Independent portable/HIP point oracles and both executors' search/fault tests
passed (six tests); BSGS search/fault tests also passed after narrowing dispatch.
The small-target xpoint compiler estimate improves from 180 to 154 VGPRs and
from two to three waves/SIMD, with zero private scratch. Retained remarks:
[xpoint](baselines/C17_MIXED_XPOINT_RESOURCES.log),
[BSGS](baselines/C17_MIXED_BSGS_SEARCH_RESOURCES.log).

**Rejected variant:** converting both BSGS groups and their caches to affine
helped smaller workloads but regressed the 32-target group-8 kernel by about
13% (auto by 11%); [raw evidence](baselines/C17_MIXED_REJECTED.json). Operation
counts alone did not predict the compiled kernel behavior. That variant is not
retained; BSGS keeps its existing cache representation and group-8 formula.
