# C23 reverse BSGS tile acceptance

Native BSGS, checkpoint runs and supervised workers support `--tile-order
forward|reverse` on HIP and CUDA. Forward remains the default. Reverse selects
the highest uncovered scalar tile within each grant. GPU giant/baby arithmetic
inside that tile is unchanged. C23 remains partial: BSGS both-ends/dance,
additional random traversal semantics and alternative minikey orders remain open.

## Decisions and coverage

This is execution policy, not a new candidate mapping. BSGS already certifies
actual scalar intervals after every canonical target subset completes. Job and
table identities, configuration version 1, schema 7, receipt encoding, protocol
and worker capabilities therefore stay unchanged. Older BSGS workers can still
process the same job forward. Scalar-search `--order` remains separate.

A restart can change tile order, giant count, target subsets, grouping kernel
and device. It walks only the complement of durable scalar coverage. Matches
from incomplete target subsets persist, but no scalar tile is credited until
all its targets complete; replay deduplicates prior matches. Reverse also orders
fragmented missing gaps and adaptive work units from highest to lowest.
Coordinator block claims retain their existing independent policy.

Non-BSGS runners reject even explicit `--tile-order forward`. `checkpoint create`
rejects the flag because it is not a job property. Native start/summary records,
checkpoint summaries and worker grant-finish events expose the selected order.
Comments explain the planner and checkpoint distinction in the implementation.
See [contract and executable example](C23_BSGS_REVERSE.md).

## Recorded validation

The native stacks used eight visible SPX/NPS1 MI300X devices and eight H200s.
Both existing BSGS kernels run in both directions; group-8 spot checks cover each
visible ordinal. This is not an exhaustive kernel-by-device product or a fleet
scaling measurement.

| Gate | Observed coverage |
| --- | --- |
| CPU release | 114/114 complete-suite gates; the subsequently added documented preparation example also passes |
| Independent integer oracle | 5,535 cases: exhaustive tiny walks, random wide intervals, checked m×giants products, near-order bounds, invalid inputs and final-baby reconstruction |
| Native CLI | 34 cases per backend: both orders and kernels, low/high/near-order bounds, single scalars, misses, dense overflow, canonical SEC1 duplicates, eight ordinals and maximum-giant short tails |
| Durable replay | Eight cases per backend: four dense overflow/group cases and four killed-process direction combinations; changed geometry, partial-target matches, owner exclusion and completed retries |
| Pause and visibility | One BSGS scenario per backend, reverse → forward → reverse, socket/signal pause, durable results, backup/restore quarantine and changed visible ordinals |
| Fragmented grants | Three accepted coverage islands, four missing gaps, expired-grant handoff from an original two-capability BSGS worker, both directions and byte-identical lost-upload retry |
| HTTPS/file workers | Two cases per backend, two grants each, exact descending work-unit unions, one executor setup and identical local/server results; file execution while the coordinator is stopped |
| Regressions | Existing native BSGS executor/failure tests on both backends; existing CPU checkpoint, control and coordinator suite |
| Sanitizers | Three focused ASan/UBSan gates: integer tile oracle, partial-target storage recovery and fragmented coordinator recovery |
| Documentation | CPU preparation and full HIP/CUDA volatile search, durable search, forward completed-job retry and journal integrity checks |

The [manifest](baselines/C23_BSGS_REVERSE_VALIDATION.json) records source commits,
binary hashes, compiler/build settings and artifact checksums.
[Algorithms](baselines/C23_BSGS_REVERSE_ALGORITHMS.json),
[recovery](baselines/C23_BSGS_REVERSE_RECOVERY.json),
[workers](baselines/C23_BSGS_REVERSE_WORKERS.json),
[examples](baselines/C23_BSGS_REVERSE_EXAMPLES.json) and
[raw logs](baselines/C23_BSGS_REVERSE_LOGS.tar.gz) retain the observations.
Only public fixtures are used; no enrollment credentials or journal databases
are archived.

## Findings and limits

The first HIP pause invocation failed while importing the shared harness's
Keccak dependency for its other mode families. Supplying the existing pinned
`KEYHUNT_KECCAK_ORACLE_ROOT` resolved collection. The unchanged production code
then passed all nine selected gates. Both the initial log and passing rerun are
retained. Existing explicit-constructor/compiler warnings remain outside this
slice.

Recovered worker journals import accepted coverage, while the prior owner's
results remain on the coordinator. The new fragmented test verifies their exact
union after upload and after a lost-reply retry. No private journal APIs were
exposed for test setup: receipts and recovery use the public coordinator protocol.

Tile partition boundaries can change with direction and geometry. Only the
completed scalar union is invariant. This adds no new GPU arithmetic, table
format, calibration, throughput improvement claim, random sampling semantics,
public deployment or additional partition certification. Sanitizers cover the
three listed host gates, not GPU device code or the entire application.

## Reproduction

Build with the existing [HIP/CUDA instructions](BUILD.md), enabling coordinator
and HTTPS support for worker gates. Use the existing Apache fixture configuration
and [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits).
Use a journal parent outside Git checkouts (`TMPDIR=/var/tmp` on the H200 host).

```sh
TMPDIR=/var/tmp KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle \
  ctest --test-dir BUILD --output-on-failure \
  -R '^(bsgs_reverse_.*|storage_bsgs_reverse|coordinator_bsgs_reverse.*|(hip|cuda)_bsgs_search(_failures)?)$'
```

The full CPU suite and the three focused sanitizer gates provide the host
coverage above. `tests/integration/bsgs_reverse_examples.py` executes the marked
blocks directly from the [public example](C23_BSGS_REVERSE.md#executable-public-example).
