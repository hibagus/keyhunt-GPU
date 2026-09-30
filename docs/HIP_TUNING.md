# C17 measured HIP tuning

C17 is complete, measured against the [C16 baseline](GPU_PROFILING.md). Each retained optimization
gets an independent commit, arithmetic/search/recovery checks, compiler evidence
where relevant, and alternating equivalent baseline/candidate trials. Raw
measurements distinguish kernel, warm executor, and fresh-process rates. BSGS
scalar coverage and target giant steps remain separate units.

## Paired trials and frozen builds

Build the HIP targets before freezing each version. Snapshots retain exact
executables, source and binary hashes, compile commands, device/topology and tool
metadata; later edits cannot replace the baseline silently. The warm benchmark
uses known outside-range targets and CPU-verifies every emitted match. It can
select either xpoint kernel alone (`--kernel stepped|direct`) and vary candidate
capacity. End-to-end trials reuse
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
settings. CLI defaults compare volatile and ten-second checkpoint execution. The
`--suite cli --workloads dense-prefix --candidate-capacity 1` case puts four
matches at the beginning of a longer sparse interval to expose persistent
overflow shrinkage.

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
separate from C17. Accepted and rejected experiments and the final validation
scope are recorded below.

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
retained; BSGS keeps its existing cache representation and group-8 formula. The
[rejected patch](baselines/C17_MIXED_REJECTED.patch), applied to `b5753be`,
reconstructs all five changed source files with hashes matching its frozen report.

## Accepted: shorter field inversion

The portable inverse computes the same Fermat exponent with 255 squares and
15 multiplies, replacing 256 squares and 249 multiplies. Define `x_k = 2^k-1`
for the exponent of each retained power. The chain builds `x2, x3, x6, x9, x11,
x22, x44, x88, x176, x220, x223`, then finishes with
`((((x223*2^23+x22)*2^5+1)*2^3+x2)*2^2+1)`. Integer substitution gives
`2^256-2^32-979 = p-2` exactly. Each square/multiply stage reads aliased inputs
before writing output. Zero still returns false and explicit zero.

[Five measured pairs](baselines/C17_INVERSE.json) show 1.417× kernel / 1.392×
warm-executor improvement for 32-target xpoint; small-target xpoint, which does
not invert, stays within 0.2%. BSGS kernels improve 1.313–1.515×, and warm
executors 1.286–1.466×. The retained direct xpoint reference improves about
1.069× at 65,536 scalars ([separate paired trial](baselines/C17_INVERSE_DIRECT.json)).

Eight arithmetic/search/fault gates passed, including 13,381 field oracle cases
and 1,278 point cases in both portable and HIP probes. These include zero,
maximal carries, independent expected inverses, batch-zero behavior and aliasing.
[Compiler xpoint](baselines/C17_INVERSE_XPOINT_RESOURCES.log) and
[BSGS remarks](baselines/C17_INVERSE_BSGS_SEARCH_RESOURCES.log) show the tradeoff:
inverting kernels use 248 VGPRs, with more private scratch and an estimated two
waves/SIMD. The reduction in arithmetic work still wins in every measured
inverting workload. No architecture-specific assembly is needed for this gain.

## Accepted: exact candidate-buffer bounds (A14)

Xpoint allocates `min(requested capacity, max steps, 2*unique X targets)` records
plus the guard. A full X has at most the two scalar preimages `k` and `n-k` in
`[1,n)`. BSGS allocates `min(requested capacity, max steps, unique public keys,64)`
plus the guard: a full public key has one scalar preimage and a batch has at most
64 targets. These bounds cannot truncate a valid nonoverflowing result. Smaller
requested capacities still overflow and discard the entire attempt as before.
Counter consistency, CPU verification and the guard remain mandatory.

[Default-capacity pairs](baselines/C17_CAPACITY.json) show warm-executor ratios
of 0.997–1.033×: this is primarily a memory/transfer improvement, not a claimed
5% default speedup. One-target downloads fall from 16,424 to 72 bytes for xpoint
and from 24,632 to 80 bytes for BSGS. At xpoint's maximum requested capacity,
[paired trials](baselines/C17_CAPACITY_LARGE.json) show 1.269–2.013× warm-executor
improvement and reduce the old 16,777,256-byte download to 72–1,064 bytes.
The measured capacity-dependent gain, predictable allocation reduction and lack
of material default regression justify retaining this below-threshold default
change. Four HIP search/fault tests pass, including both `n/2` sign matches,
capacity-one overflow, repeated 64-target subsets and guard corruption.

## Accepted: recover xpoint batch size after overflow (A12)

Both volatile and durable owners share a bounded policy. Overflow discards the
attempt and reduces the limit to `min(capacity, attempted steps/2)`. Since unique
X targets emit at most one candidate per scalar, the next attempt fits even on
fully dense input. Successful attempts at most half full double the limit, up to
the original configured maximum; full buffers keep their stable smaller size.
Each success advances exact coverage, and every overflow is followed by a fitting
attempt. Recovery is in-memory tuning, not persisted coverage or target state.

The [alternating dense-prefix comparison](baselines/C17_RECOVERY.json) searches
4,095 scalars with four initial matches, 256-scalar work units and capacity one.
Both versions execute 4,351 attempted steps and accept exactly 4,095 useful steps,
all four matches and one discarded overflow. Launches drop from **4,096 to 28**
in both volatile and durable modes. Median warm-executor speedups are 25.427×
and 28.841×; whole-process speedups are 1.350× and 1.464× because startup still
dominates the short optimized run. These are dense-prefix-specific results.

Seven focused tests pass: both xpoint CLI kernels, durable checkpoint execution,
fault/recovery, owner controls and HIP pause. New fixtures require large batches
to return across later work units, retain exact coverage/results, and keep fully
dense capacity-one input at one initial overflow rather than repeated retries.

## Corrected: mixed durable BSGS metrics (A19)

The independent [C16 audit](audits/C16_AUDIT.md) found that durable summaries
reported only the last grouping kernel. Version 2 now accumulates launch counts,
overflows, attempted/useful target giant steps and kernel time separately for
groups 1 and 8. The old scalar is explicitly last-dispatch information. Benchmark
validation checks group totals against the aggregate and reports the complete
set as unknown for frozen version 1 executables. This changes diagnostics only.

Six focused metric/checkpoint/failure tests pass, plus a rerun with visible-CU
adapted integration geometry. Cases cover overflow into another group, mixed
target subsets and a final short tile. The [live audit reproduction](baselines/C17_GROUPS.json)
passes five measured repetitions plus warm-up for volatile and timed modes, each
with exact coverage and 524,288 useful steps. Durable output correctly retains
two group-8 and two group-1 launches instead of labeling the run only group 1.

## Final C16 → C17 comparison

The [final matrix](baselines/C17_MEASUREMENTS.json) compares unchanged C16
production binary `a2823f732401…` with C17 `1fe9412d0a0d…` (code commit
`863da3d`). It contains five measured alternating process pairs after one excluded
warm-up pair: 180 measured CLI executions plus 36 warm-ups, and separate warm
executor samples. Every CLI execution passes independent target-oracle, exact
coverage and match checks; durable executions also reopen and audit the journal.

Measured on one MI300X gfx942, 304 visible CUs, SPX/NPS1, wave64, physical PCI
`0000:3d:00.0` under `HIP_VISIBLE_DEVICES=1` (runtime ordinal 0). ROCm compiler
flags, exact binary/source hashes, CPU affinity, clocks/power snapshots and all raw
commands/samples are in the report. The host is unreserved. No hardware settings
were changed; the final timing window contains no regression or profiling jobs.

| Workload | Kernel speedup | Warm executor speedup | Volatile process | Timed process | Every-batch process |
| --- | ---: | ---: | ---: | ---: | ---: |
| xpoint no-match-1 | 1.936× | 1.873× | 1.234× | 1.255× | 1.255× |
| xpoint boundary-3 | 1.981× | 1.905× | 1.286× | 1.265× | 1.247× |
| xpoint no-match-32 | 2.330× | 2.281× | 1.648× | 1.626× | 1.538× |
| bsgs no-match-1 | 1.694× | 1.663× | 1.132× | 1.155× | 1.143× |
| bsgs boundary-3 | 1.314× | 1.309× | 1.089× | 1.107× | 1.094× |
| bsgs no-match-32 | 1.328× | 1.347× | 1.134× | 1.138× | 1.114× |

Ratios are medians of paired baseline/candidate elapsed times, so greater than
one is faster. Xpoint uses 1,048,576 scalars per warm batch; BSGS uses `m=65537`,
32,768 giants/target and automatic grouping (1 for the one-target case, 8 for the
others). Explicit group-1 and group-8 warm samples are retained too; their kernel
speedups range from 1.308× to 1.717×. Each CLI interval spans 512 bounded batches
with an exact final tail. BSGS steps count target giants, not scalar coverage.

Every warm kernel pair improves by at least 28.8%. Whole-process results include
HIP startup, table preparation, output, verification, cleanup and storage, so
they are smaller and noisier. Four BSGS process comparisons include individual
pairs below 1× (down to 0.940×), despite improved medians; their process gains
should not be treated as guarantees outside this host/workload. Xpoint process
medians improve 1.234–1.648×, with every individual pair improving. The short
512-batch timed runs need only final/match commits; they do not establish long
periodic-checkpoint or multi-hour sustained rates. The C16 cadence evidence
remains historical validation of that unchanged mechanism.

The [separate final profile](baselines/C17_PROFILES.json) validates mixed BSGS
dispatch counts and retains compiler-generated assembly/resource reports for
both search translation units. Its instrumented timings are excluded from the
benchmark. Register/private-memory numbers are compiler estimates, not measured
occupancy. No handwritten ISA was retained: portable arithmetic and launch/buffer
changes clear the measured gate; architecture-specific specialization remains C19.

## Pause, validation and limits

The [baseline pause run](baselines/C17_PAUSE_BASELINE.json) and
[final pause run](baselines/C17_PAUSE.json) each take five measured warm samples
after excluding startup, for small/default xpoint and BSGS geometry. Baseline
median request-to-durably-paused times are 0.628/0.624 ms for xpoint and
0.803/1.059 ms for BSGS. C17 medians are 0.456/0.534 ms and 0.404/0.415 ms; its
maximum observed value is 0.774 ms. These are separate finite sample runs, not
alternating latency pairs or universal response-time limits. Every saved interval
union agrees with the stopped owner's accepted coverage, and journal checks pass.

The [acceptance manifest](baselines/C17_VALIDATION.json) records **33/33 CPU
release, 59/59 HIP/coordinator, 8/8 focused debug and 6/6 focused address/undefined
sanitizer tests**, all passing. Debug/sanitizer gates include the changed portable
arithmetic and checkpoint paths. HIP regression validation used the same one
visible SPX/NPS1 device; concurrent CPU checks started only after timing, pause
and profiling finished. Source and binary hashes match the final frozen build.

The [evidence check](baselines/C17_EVIDENCE_CHECK.json) independently recalculates
1,388 paired statistic distributions, verifies 1,401 command stdout/stderr hash
pairs and 20 profiler artifacts, and checks all final production source hashes.
[Raw logs and compiler artifacts](baselines/C17_RAW_LOGS.tar.gz) retain 2,913
files under their `keyhunt-c17-*` temporary-directory prefixes, including the
validation/check scripts and frozen snapshot metadata. Runtime journals, table
caches and executable copies are excluded. Report paths map to the matching
archive prefixes; rebuild executable snapshots from their recorded commits,
source hashes and compile flags before repeating performance trials.

C17 closes the standalone performance findings A12/A13/A14 and diagnostic A19.
It does not establish a global hardware optimum, sustained multi-hour rates,
multi-GPU scaling, CUDA performance or supervisor-level throughput. Existing
A16/A17/A18 supervisor lifecycle findings remain tracked in the
[C15 audit](audits/C15_AUDIT.md); standalone pause validation does not close them.
Broader ISA specializations, cooperative inversion/group-size experiments and
multiple-device scheduling retain their own gates.
