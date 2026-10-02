# C23: scalar random-window validation

Acceptance: PASS. This closes C23 for the documented exact GPU interfaces:
named search families, scalar strides/reverse/orbits, GLV and execution policies
for BSGS, minikeys and scalar batches. The
[random-window contract](C23_SCALAR_RANDOM_WINDOW.md) defines the final slice and
contains its executable public-address example. Historical CPU limitations and
inactive `pub2rmd` remain documented in [MODES.md](MODES.md).

Validated source: `648e9092e8e333e7fd470b9a130e097ca2e8885a`, after scalar dance
acceptance at `b9b2f3de5916b4bb0ac0e900a77cd9b2edcf993d`. The final acceptance commit
only records evidence and updates documentation. There is no throughput or
search-probability claim.

## Recorded checks

| Build | Passed gates | CTest wall time |
| --- | ---: | ---: |
| CPU Release, full suite | 195 | 192.42 s |
| Host Debug, ASan/UBSan | 9 | 888.91 s |
| HIP Release, focused suite | 22 | 410.91 s |
| H200 CUDA Release, focused suite | 22 | 2610.69 s |

Every recorded gate passed without a skip or timeout. The full CPU suite covers
prior mode families and SHA-256 consumers. Focused hardware suites include the
new scalar window policy, both independent scalar planners, work-unit checks,
existing stride/reverse/GLV executors and host both-ends/dance recovery. Hardware
gates serialize access to their test device. The host sanitizer suite covers
both oracles, work units, all three non-forward scalar recovery policies and the
corresponding coordinator fixtures.

- The independent integer/hashlib oracle checked 2,500 random-window cases with
  39,445 unaccepted attempts and 10,762 rejected random draws. It observed 72 live
  owners and 69 extra accepted fragments, within the configured bounds. Cases
  cover fragmented gaps, seed zero and the 256-bit endpoint, windows 1 through
  256, changed work/batch bounds, UINT64_MAX reservations, huge domains, retries,
  empty coverage, invalid settings, unmapped scalars, strides, reverse mappings
  and six-member orbits. It verifies complete submitted sequences, scalar values,
  owner announcements/completion and disjoint coverage. The prior 2,174-case
  forward/both-ends/dance oracle also passes.
- Each backend passed 607 native cases: 203 forward strides, 203 reverse strides,
  103 forward orbits and 98 reverse orbits. Xpoint, HASH160, Bitcoin address,
  Ethereum and vanity run through direct, stepped and GLV kernels. Independent
  result sets and every attempted interval, including overflow, are checked.
  Profiles include default settings, seed `2a`/window 4, maximal seed/window 1
  and a wide seed/window 256. All eight visible GPUs receive spot checks.
- The host fixture passed 560 random-window recovery combinations: four families,
  five mappings, fixed/adaptive work, seven policy transitions and two fault
  points. Tests interrupt before receipt and after durable commit, then recover
  the exact complement with changed geometry, seed and window. They check retry
  endpoints, pauses, owner announcements, verified relations, backups and
  zero-work completed retries. All 240 both-ends and 400 dance combinations also
  pass.
- Each backend passed 56 GPU checkpoint cases, including eight killed-process
  restarts, and four pause/restore cases. Shuffled partial coverage survives a
  seed/window change. Pause recovery switches policy, geometry and visible
  devices without changing the immutable scalar mapping or result union.
- Each backend passed 16 worker cases over real HTTPS and disconnected courier
  files, both scalar mapping orders and all four families with orbit expansion.
  Transport settings use seed `2a`/window 4 and maximal seed/window 256; completion
  telemetry must match. Checks include grant boundaries inside a variant, warm
  executor reuse, canonical import, local/server result parity, byte-identical
  export retries, duplicate import, acknowledgments and cleared outboxes. The
  host worker fixture verifies frozen independent permutations on two grants and
  lost-upload-reply recovery. Invalid supervisor settings fail before state opens.
- The public example runs verbatim. Its seed-42/window-4 six-tile sequence matches
  the contract, scalar 1 is found once, and a completed forward retry submits no
  work. CPU runs the preparation block; HIP/CUDA execute both blocks. Native and
  durable summaries expose the expected seed/window, and forward omits them.

## Decisions and findings

Window construction takes ascending missing coordinates, fixes tile geometry and
contiguous ownership, then shuffles. This keeps memory bounded by W tile records
and W owners beyond the input gaps. It is local randomization within ascending
windows, not uniform sampling across the entire domain or sampling with replacement.

The SHA-256 domain `khscalar-window-v1` plus its terminating zero byte and explicit
big-endian seed/counter make ordering portable. A shared SHA-256 library lets the
scheduler reuse the existing primitive without depending on the legacy curve
engine. Its implementation is unchanged; the full host suite validates consumers.

A shuffled tile can cross an orbit variant boundary. KernelBatch still clips each
submission to one variant. Completion therefore uses the actual accepted end,
not the requested step count, and the tile suffix remains selected until exhausted.
Overflow never draws again or changes the unaccepted start. Growth is clipped to
the tile boundary. Owner timing excludes pauses and execution of other owners.

Restart intentionally resets the stream over the exact saved complement. Seed,
window, policy, geometry and device are execution settings. Existing scalar
mappings, job/configuration identities, schema 7, receipts, protocol and
capabilities remain unchanged. Creation, BSGS and minikey runners reject scalar
batch settings, and explicit seed/window options require random-window.

No production or test failures were observed in the recorded acceptance runs.
The evidence establishes bounded exact coverage and result parity on synthetic
public fixtures; it does not establish a production-scale performance benefit.

## Evidence and reproduction

The [manifest](baselines/C23_SCALAR_RANDOM_WINDOW_VALIDATION.json) binds source,
selected tests, build/compiler settings, binary hashes and raw evidence hashes.
Grouped reports retain [algorithms](baselines/C23_SCALAR_RANDOM_WINDOW_ALGORITHMS.json),
[recovery](baselines/C23_SCALAR_RANDOM_WINDOW_RECOVERY.json),
[workers](baselines/C23_SCALAR_RANDOM_WINDOW_WORKERS.json) and
[examples](baselines/C23_SCALAR_RANDOM_WINDOW_EXAMPLES.json). The
[raw archive](baselines/C23_SCALAR_RANDOM_WINDOW_LOGS.tar.gz) contains selections,
JUnit results, logs, build records, reports and collection scripts. Example
reports bind the contract document by SHA-256.

Hardware was eight MI300X devices in SPX/NPS1 mode (gfx942, ROCm Core 10.0,
GFX942 carry enabled) and eight NVIDIA H200 devices (sm90, CUDA 13.3). Release
builds include the coordinator and HTTPS worker. The separate host Debug build
enables ASan/UBSan. Exact compiler versions are retained in the manifest.

Replay individual recorded `selection.json` commands, or run
`ctest --test-dir BUILD -R 'scalar_random_window|scalar_batch_oracle' --output-on-failure`.
The host fault corpus is `storage_scalar_both_ends_test --random-window`.
Tests use journals outside the checkout (`TMPDIR=/var/tmp`), the pinned Keccak
oracle, and an Apache fixture for worker transports. Resolved paths, environment
settings and full suite filters are in the archived scripts and commands.
