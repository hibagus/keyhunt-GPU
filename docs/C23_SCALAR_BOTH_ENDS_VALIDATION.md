# C23 both-ends scalar batch acceptance

Status: complete on the recorded MI300X and H200 stacks. The [contract and public
example](C23_SCALAR_BOTH_ENDS.md) describe the exact traversal rules. All four final
validation suites passed without failures or timeouts.

## Implementation

`--batch-order both-ends` alternates low and high missing batches for xpoint,
Bitcoin address/HASH160, Ethereum and vanity. Direct, stepped and GLV kernels
keep their existing scalar mapping. Positive strides, immutable reverse mapping
and six-member orbit expansion remain supported. High orbit batches are clipped
at the lower boundary of their last variant.

The new shared host planner separates reservation from accepted coverage. Failed
overflow attempts retain their endpoint; accepted batches remove exactly a prefix
or suffix. Two active work reservations suffice, including when fronts meet inside
one owner. Work accounting charges that owner's execution and replay only.
The forward path now also observes completed work at gap ends when updating
adaptive size; this can change later work geometry without changing coverage.

Batch order is execution-only. Native commands, checkpoint runs, persistent
workers and the Python supervisor expose it. Job identities, target/configuration
bindings, schema 7, receipt bytes and capabilities are unchanged. Restart may
switch batch order while retaining the immutable scalar mapping. Completed retries
submit nothing. BSGS, minikeys and checkpoint creation reject explicit overrides.

## Validation runs

| Build | Gates | Result |
| --- | ---: | --- |
| CPU Release, full suite | 172 | Passed |
| Host ASan/UBSan | 10 | Passed |
| MI300X HIP Release | 29 | Passed |
| H200 CUDA Release | 29 | Passed |

CPU validation took 116.55 seconds; sanitizer validation took 398.88 seconds,
including the 240-case recovery fixture in 384.67 seconds. HIP validation took
582.99 seconds; CUDA validation took 3,997.48 seconds. These durations include fixture setup and fresh CLI launches;
they are not search-throughput measurements. All final runs use source `0d2704b`; subsequent changes record
acceptance evidence only. Build metadata includes exact executable hashes,
compiler versions and CMake settings.

Each GPU backend runs 592 new native cases (188 forward-stride, 203
reverse-stride, 103 forward-orbit and 98 reverse-orbit), 56 checkpoint cases
including eight killed restarts, four pause/restore cases, 16 HTTPS/file worker
cases and the executable public example. Default-order regression corpora and
executor contracts are also included. CPU-only CLI reports are preparation and
rejection checks; they are not counted as hardware cases.

## Independent and durable checks

The integer oracle checks 1,447 cases and 8,533 unaccepted attempts. It uses a
separate list of missing intervals and immutable owner reservations, and checks
every submitted interval, owner boundary, start/finish flag, endpoint scalar and
orbit variant. Small complete domains, fragmented gaps, changing batch/work
limits, 64-bit work limits, wide coordinates, order boundaries and malformed
inputs are included. The observed maximum is two live owners.

Native GPU fixtures check the entire submitted sequence against that model,
including overflow attempts and subsequent size growth. They independently derive
public keys with the pinned libsecp256k1 oracle and construct expected targets and
relations for all four scalar families, with Bitcoin address parsing also covered.
The corpus exercises both immutable mapping orders, all three kernels, unit and
nonunit strides, wide origins/steps, near-order bounds, no-hit targets, overlapping
relations, orbit boundaries, maximum batches and all eight visible device ordinals.

The host recovery fixture checks 240 combinations: four families, five mapping
configurations, fixed/adaptive work, three restart policy transitions, and faults
before receipt or after durable commit. Every coordinate in the small fixture is
targeted; real overflow, exact missing-set consumption, pause, backup, deduplicated
results and completed retries are asserted. GPU checkpoints add dense overflow,
wide mappings, overlapping orbits and killed-process recovery. Control tests
exercise both-ends to forward to both-ends across pause, restore and device
visibility changes.

Worker fixtures cover both immutable orbit orders over HTTPS and disconnected
courier files. Each executes two grants with one prepared executor, checks exact
local/server relations and coverage, retries identical exports and duplicate
imports, and confirms execution while the server is stopped. The host worker
fixture additionally checks endpoint order and lost upload acknowledgements for
all four families.

## Reproducible evidence

The [manifest](baselines/C23_SCALAR_BOTH_ENDS_VALIDATION.json) records source phases,
exact binary hashes, compiler/build settings, selected gates, durations and all
artifact checksums. The tested source is
`0d2704b6f3ae15d240d9d51a7d8eaeaa14d2f2f4`, following the previous acceptance at
`a34ab2f8822bd703627d92a4bb1e80a197816c65`. The final acceptance commit changes
only this document and its evidence files.

- [Algorithm cases](baselines/C23_SCALAR_BOTH_ENDS_ALGORITHMS.json): integer oracle and native HIP/CUDA sequences/results.
- [Recovery cases](baselines/C23_SCALAR_BOTH_ENDS_RECOVERY.json): dense checkpoints, killed restarts and pause/restore.
- [Worker cases](baselines/C23_SCALAR_BOTH_ENDS_WORKERS.json): HTTPS and disconnected courier execution, exact local/server relations and warm grant handoff.
- [Executable example](baselines/C23_SCALAR_BOTH_ENDS_EXAMPLES.json): commands executed verbatim, bound to the contract's SHA-256.
- [Raw evidence](baselines/C23_SCALAR_BOTH_ENDS_LOGS.tar.gz): CTest selections, JUnit results, detailed logs, build metadata, collection scripts and retained development failures.

The local host exposed eight MI300X SPX/NPS1 devices; HIP used gfx942 with the
existing carry specialization enabled. The remote host exposed eight H200 devices;
CUDA used sm90. Inventory and compiler/runtime details are in the reports. Each
native corpus includes visible-device spot checks. These checks establish neither
fleet scaling nor comparative throughput.

## Findings and scope

An early recovery test exposed a production edge case: finished checkpoints have
executor generation zero because they do not start a new executor. The planner
now validates a live work identity only when missing coverage exists; completed
retries remain read-only inspections. This fix is in `88df111` and all final
validation uses it.

Development logs also retain a missing standard-header build failure and two
fixture corrections: pausing before completion rather than after it, and reading
all results rather than the default 100-row page. An initial empty CTest regular
expression selected no CPU tests; the full 172-gate run used an explicit match-all
expression. These attempts are not counted as acceptance passes.

This is a host traversal change with no new GPU arithmetic or performance claim.
Reported hardware acceptance applies to the recorded MI300X/gfx942 and H200/sm90
builds. C23 remains partial: scalar dance and random-window traversal are pending.
