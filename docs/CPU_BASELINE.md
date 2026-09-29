# C01: CPU behavior and hardware baseline

Recorded 2026-09-29 against `b1066ab`; production sources are unchanged from
`2134a20`. C01 adds a characterization harness, public synthetic fixtures,
environment capture, and a finite benchmark. It changes no search algorithms.
Implementation status is tracked in [IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md).
C02 subsequently resolved the legacy build gate with isolated GMP development
files; see [BUILD_MIGRATION.md](BUILD_MIGRATION.md). C01 records below retain
the original observation before that dependency was supplied.

## Reproduce

Build the original CPU target with `make`, then run from the repository root:

```sh
python3 tests/baseline/run_cpu_baseline.py --binary ./keyhunt --report /tmp/cpu-results.json
python3 tools/capture_environment.py --binary ./keyhunt --output /tmp/environment.json
python3 tools/benchmark_cpu_baseline.py --binary ./keyhunt --report /tmp/benchmark.json
```

Each case runs in its own temporary directory, with a timeout and synthetic
public test targets. Result files and caches cannot overwrite a user's search
state. The suite requires Python 3.9 or newer and no third-party Python modules.
It checks exit status, matches in result files and stdout, compression encodings,
and selected diagnostics. The minikey and random tests deliberately terminate
ongoing searches at their time limits; other timeouts are failures. The suite
runs one search thread per case and is sequential.

`tests/baseline/vectors.json` is committed, so running tests does not need any
crypto package. `python3 tests/baseline/generate_vectors.py --check` reproduces
its contents using a small affine Python integer implementation and SHA-256 /
RIPEMD-160. On this machine Python lacks RIPEMD-160, so the optional generator
uses the system OpenSSL 3 CLI with its legacy provider explicitly loaded for
that subprocess. It makes no global OpenSSL configuration change. The scalar-1
Bitcoin addresses are checked against fixed known values; Ethereum scalar 1 is
a fixed fixture. This generator is a transparent smoke-test reference, **not**
the independently pinned arithmetic oracle required by C06.

For this capture, the unchanged tree was exported with `git archive HEAD` into
an isolated temporary directory and built there. Build outputs are not tracked.
The JSON records identify the tested executables by SHA-256; generated timing
records should be deliberately reviewed before committing new snapshots.

## Build and environment results

| Check | Observed result |
| --- | --- |
| Default `make` | Passed with GCC/G++ 11.4.0; original `-Ofast`, native x86, SSSE3 and mixed LTO flags retained |
| `make bsgsd` | Passed; original unused-parameter warning retained |
| `make legacy` | Failed: `gmp256k1/Int.h` cannot find `gmp.h`; GMP development headers are absent |
| Python / CMake | 3.10.12 / 3.22.1 |
| CPU | Intel Xeon Platinum 8468; 96 visible logical CPUs, 2 sockets, 48 cores/socket, 1 visible thread/core |
| ROCm Core / HIP compiler | Core 10.0.0; HIP 7.15.26333; AMD clang 23.0.0git |
| ROCm enumeration | 64 logical `gfx942` MI300X agents, each 38 CUs and wavefront size 64 |
| Partition metadata, agent 0 | CPX, NPS4, CAPPING |
| NVIDIA compiler | `nvcc` unavailable on PATH |

The legacy dependency failure is recorded, not a successful compatibility test.
C02 must detect GMP/OpenSSL only when the optional legacy target is requested,
with an actionable configuration diagnostic. A successful legacy build and
fixture run remain required when those dependencies are available. The daemon
was built but not started for this milestone; its loopback protocol integration
belongs to the build/migration checks in C02.

These are logical GPU agents, not 64 physical cards. HIP allocation limits,
physical package mapping, kernel execution, and runtime compatibility remain C07
work. No GPU benchmark or GPU correctness claim is made by C01.

Raw artifacts:

- [CPU test results](baselines/C01_CPU_RESULTS.json)
- [Environment capture](baselines/C01_ENVIRONMENT.json)
- [Main build log](baselines/C01_BUILD_MAIN.log)
- [Legacy build log](baselines/C01_BUILD_LEGACY.log)
- [BSGSD build log](baselines/C01_BUILD_BSGSD.log)
- [Finite xpoint timing samples](baselines/C01_BENCHMARK_XPOINT.json)

## Characterized behavior

The 38 checks cover xpoint, Bitcoin address, HASH160, Ethereum address, vanity,
minikeys, BSGS, disabled pub2rmd, CLI errors, and range/stride edge cases. Both
compressed and uncompressed Bitcoin encodings are exercised. A range above
2^80 exercises the wide scalar path. Single-threaded deterministic cases avoid
depending on scheduling or output ordering; result multiplicity is checked.

### Range endpoints and selection

With no `-R`, the ordinary CPU worker chooses sequential batch bases. It tests
`base < end`, then scans the **entire** `-n` batch. With stride 1, an aligned
range behaves as `[start,end)`. A partial final batch is not clipped:

```sh
# Targets include synthetic scalars 0x1000, 0x1001, 0x13ff, 0x1400,
# 0x1401 and 0x17ff. The harness writes their x coordinates to targets.txt.
./keyhunt -m xpoint -f targets.txt -r 1000:1401 -n 1024 -t 1 -q -s 0
```

This finds all six targets, including `0x1401` and `0x17ff` past the requested
end. Even `-r 1000:1001` scans through `0x13ff`. Conversely, `1000:1400`
finds `0x1000` and `0x13ff`, but excludes `0x1400` as expected for an aligned
batch. These are tested observations, not an exhaustive correctness proof.

Further observations:

- Zero start is normalized to 1. Reversed bounds are silently swapped after a
  warning. Equal bounds print a fallback warning and select the full scalar
  range; the test then supplies a missing file to stop before computation.
- `-n` affects sequential batching despite help describing it primarily with
  `-R`. Non-BSGS values below 1024 or not divisible by 1024 revert to 2^32.
  That fallback is identified in source, not exercised as a long search.
- `-I 2` advances candidates by 2 but reserves subsequent batch bases by `-n`,
  not `2 * -n`. For `1000:1800 -n 1024`, `0x1400` and `0x17fe` appear twice,
  and `0x1800` / `0x1bfe` are reported outside the requested range. The suite
  checks multiplicity to preserve this evidence during mechanical moves.
- `-R` repeatedly samples bases, without an exhaustive coverage journal or a
  deterministic seed exposed by the CLI. Its test checks bounded observation
  of continued execution, not a reproducible sequence or unique coverage.
- Endomorphism and all randomized BSGS selection paths have not been certified
  for coverage. They must not inherit new journal semantics without an audit.

Source evidence at the baseline revision: `keyhunt.cpp` range parsing around
lines 816–843, batch reservation around 2548–2565, candidate iteration around
3060–3097, and BSGS reservation around 3820–3844.

### BSGS boundary defects

Tests use `-n 1048576` (M=1024) and `-k 1` (the default), keeping setup small.
A range smaller than N is rejected. Exact-square-root and M-divisibility
requirements are observed in the parser.

For sequential BSGS, `-r 100000:300000` misses a target at **exactly**
`0x100000`, while finding `0x100001`. For the non-aligned range
`100000:200001`, it also finds targets at `0x200001`, `0x200002`, and
`0x300000`, beyond the declared end. These are reproduced against generated
public points, including exact result public-key checks in positive tests.
The implementation reserves bases in increments of `2*N`; base checks alone
do not enforce candidate bounds. Finding every target exits with status **1**;
exhausting the selected range without finding every target exits **0**.

The baseline explicitly names these cases `*_existing_defect`. Their expected
outputs capture existing behavior for C02, not desired behavior for the new
engine. C05/C06/C09/C11 must add separate strict half-open coverage tests and fix
or isolate incompatible legacy behavior. Never credit these CPU loops with
exact range completion merely because they exited successfully.

### Other compatibility details

- `-h` prints help and exits 1; invalid mode, cryptocurrency and unknown options
  also exit 1. Missing target files fail.
- Empty and malformed xpoint input can load zero targets, run, and exit 0.
  These are marked known defects; a successful exit is not input validation.
- `pub2rmd` prints that it was removed and exits 0. It is not an active mode in
  this main executable, despite the old README's experimental-mode listing.
- Minikey search uses a separate candidate domain. A specified base is
  incremented before checking; a fixed public base produces the known first
  valid match. It does not terminate after a match or exhaust a scalar `-r`
  interval. `-n` is an allocation size, not a total search limit.
- Results are appended to `KEYFOUNDKEYFOUND.txt` or `VANITYKEYFOUND.txt` in the
  working directory. Temporary test directories contain these side effects.
- BSGS `-S` stores precomputation caches, not search progress. No durable search
  checkpoint or coordinator exists yet.

## Benchmark methodology and first measurement

`tools/benchmark_cpu_baseline.py` performs a warmup followed by three measured
runs, each in a fresh temporary directory. The initial xpoint workload uses one
CPU thread, `-n 1048576`, and a nominal width of 2^24 from `0x100000`, with a
known target (scalar 1) below the range. It asserts normal termination and no
matches. No shared precomputed files are present.

Observed process wall times: warmup **3.033286 s**; measured **3.033192 s**,
**3.033008 s**, **3.033100 s**; median **3.033100 s**. These include startup,
target setup and the main loop's one-second shutdown polling. This short run is
a repeatable build-regression workload, **not a precise kernel throughput
measurement**. Do not infer a speedup from sub-second differences here or use the
application's inflated keys/s counters as unique-coverage evidence. Other system
load, CPU affinity, frequency and power were not controlled.

For later comparisons, keep mode, target set, range, threads, compiler flags,
encoding, stride and endomorphism identical; record repeated raw samples and the
binary hash. Increase the workload so polling/setup are small relative to useful
work. Measure setup, arithmetic, lookup, verification, I/O and shutdown separately
once C04 exposes those boundaries. GPU timing must additionally include device
identity, partitions, synchronization and transfer costs under C16. Correctness
and complete coverage remain independent gates from timing.

## C06 behavior correction

The C01 artifacts above preserve the original implementation's observations.
C06's [independent point checks](ARITHMETIC_ORACLE.md) fixed infinity, equal-point
addition and zero-scalar handling. The current CPU BSGS test now finds scalar
`0x100000` at the beginning of `[0x100000,0x300000)`, returning the established
all-found status 1. Its regression case is now `bsgs_start_boundary`.
The tail-overrun case also finds `0x100000` in addition to its previous results;
it still reports out-of-range tail candidates and remains a known-defect test.
[The original expectation failures](baselines/C06_POINT_BASELINE_CHANGE.json)
record this intentional change. All other 36 cases retain their expectations.
These corrections do not certify the legacy search loops as exact coverage.
