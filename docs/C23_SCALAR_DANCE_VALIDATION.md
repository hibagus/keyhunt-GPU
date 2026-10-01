# C23: scalar dance validation

Acceptance: PASS for the fixed-midpoint scalar dance slice. C23 remains partial;
scalar random-window traversal is still pending. The behavior and executable
public-address example are defined in [the contract](C23_SCALAR_DANCE.md).

Validated source: `c96dc0fcf12d58ac1c43b87a35e485f46211b8c4`, after scalar both-ends
acceptance at `f94a46b992924497cf7040484d920bc046a5aaa6`. The final acceptance commit
only records evidence and updates documentation. No performance claim is made.

## Recorded checks

| Build | Passed gates | CTest wall time |
| --- | ---: | ---: |
| CPU Release, full suite | 183 | 133.00 s |
| Host Debug, ASan/UBSan | 6 | 627.83 s |
| HIP Release, focused suite | 19 | 396.17 s |
| H200 CUDA Release, focused suite | 19 | 2040.40 s |

The full CPU suite includes prior C23 families and scalar batch-order regressions.
The focused hardware suites include dance, the existing stride/reverse/GLV
executor gates, the independent planner, work-unit checks and both-ends host
recovery. GPU tests serialize access to their test device. Sanitizers exercise the
planner, work units, both scalar batch policies and both coordinator fixtures.
Every recorded test passed without a skip or timeout.

- The independent integer model checked 2,174 planner cases and 13,075 unaccepted
  attempts. It reached three simultaneous owners. Cases include fragmented gaps,
  pivot-in-hole and pivot-at-edge selections, odd/even envelopes, upper-half
  exhaustion, empty coverage, changing bounds, UINT64_MAX reservations, wide
  coordinates, curve-order edges, unit/nonunit strides, reverse mappings and all
  orbit variants. Expected scalar values and reservations are independent of the
  production interval map.
- Each backend passed 607 native cases: 203 forward strides, 203 reverse strides,
  103 forward orbits and 98 reverse orbits. They cover xpoint, HASH160, Bitcoin
  address, Ethereum and vanity through direct, stepped and GLV kernels. Batch
  intervals, attempted overflows and result sets are checked independently.
  Spot checks run on all eight visible devices per host.
- The host recovery fixture passed 400 dance combinations: four families, five
  mappings, fixed/adaptive work, five policy transitions and two fault points.
  It checks crashes before receipt and after durable commit, exact complements,
  phase endpoints, pivot clipping, pauses, owner announcements, results, backups
  and zero-work completed retries. The prior 240 both-ends combinations also pass.
- Each backend passed 56 GPU checkpoint cases, including eight killed-process
  restarts, and four pause/restore cases with changed device visibility and
  geometry. Recovery can switch between dance, forward and both-ends policies
  while retaining the immutable scalar mapping.
- Each backend passed 16 worker cases across real HTTPS and disconnected courier
  files, both scalar mapping orders and all four families with orbit expansion.
  Checks cover grant boundaries inside an orbit variant, warm executor reuse,
  local/server result equality, byte-identical export retries, duplicate import,
  durable acknowledgments and an empty acknowledged outbox. The host worker
  fixture also verifies midpoint endpoints and lost-upload-reply recovery.
- The public example runs verbatim: scalar 1 is found once, the exact six-batch
  dance sequence matches the contract, and a completed forward retry submits no
  work. CPU validation runs its preparation block; HIP/CUDA execute both blocks.

## Decisions and findings

Dance is an execution policy over existing receipt coordinates. The fixed pivot
splits missing coverage before work reservations are made; this allows a map
lookup for the middle phase and prevents a reservation from straddling the pivot.
The phase advances only after acceptance. High-side orbit clipping remains in
place so a batch never mixes variants. Each of the at most three owners retains
its original bounds and accrues only its own active execution/replay time.

Restart deliberately recomputes the pivot from the saved complement and begins
low. Phase and midpoint are not persisted. Existing scalar mappings, job IDs,
schema 7, receipts, coordinator protocol and capabilities remain unchanged.
Compatible older workers can continue in forward batch order. Explicit scalar
batch-order options remain invalid for creation, BSGS and minikey searches.

No production or test failures were observed in the recorded acceptance runs.
The native forward corpus includes an explicit unmapped unit-stride dance case
for every family/kernel, in addition to mapped stride and orbit coverage.

## Evidence and reproduction

The [manifest](baselines/C23_SCALAR_DANCE_VALIDATION.json) binds the tested source,
selected tests, compiler/build settings, binary hashes and every raw evidence file.
Grouped reports retain [algorithms](baselines/C23_SCALAR_DANCE_ALGORITHMS.json),
[recovery](baselines/C23_SCALAR_DANCE_RECOVERY.json),
[workers](baselines/C23_SCALAR_DANCE_WORKERS.json) and
[examples](baselines/C23_SCALAR_DANCE_EXAMPLES.json). The
[raw archive](baselines/C23_SCALAR_DANCE_LOGS.tar.gz) contains CTest selections,
JUnit results, logs, build records, reports and the collection scripts.
The example reports also bind the contract document by SHA-256.

The hardware hosts were eight MI300X devices in SPX/NPS1 mode (gfx942, ROCm Core
10.0, GFX942 carry enabled) and eight NVIDIA H200 devices (sm90, CUDA 13.3).
Release builds enable the coordinator and HTTPS worker; the separate host Debug
build enables ASan/UBSan. The exact compiler versions are in the manifest.

Use the recorded `selection.json` commands to reproduce individual tests, or run
`ctest --test-dir BUILD -R 'scalar_dance|scalar_batch_oracle' --output-on-failure`.
The host fault corpus is `storage_scalar_both_ends_test --dance`. Tests require
journals outside the checkout (`TMPDIR=/var/tmp`), the pinned Keccak oracle, and
an Apache fixture for HTTPS/courier gates; their resolved paths are in the
recorded test commands. The raw validation script contains the complete suite
filters and environment settings.

This acceptance uses synthetic public fixtures and exact coverage/result checks.
It does not establish production-scale throughput or a discovery-probability
benefit. Scalar random windows remain a separate C23 acceptance task.
