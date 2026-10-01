# C23: both-ends scalar batches

Status: implementation and validation in progress.

`--batch-order forward|both-ends` selects execution order for xpoint, Bitcoin
address/HASH160, Ethereum and Bitcoin vanity. It is available on native searches,
checkpoint run, worker run-device and the Python supervisor. Default is forward.
Checkpoint creation, BSGS and minikeys reject it, including explicit forward.

This execution setting does not change `--order forward|reverse`, which is an
immutable scalar mapping. Work is selected in the job's existing receipt
coordinates: actual scalars for unmapped searches, candidate indices for strides,
reverse mappings and six-member orbits. No identity, configuration, schema,
receipt, protocol or capability version changes. Older compatible workers can
continue the same job in forward batch order.

Both-ends begins low and alternates low/high after each accepted batch. Each side
reserves a contiguous adaptive work unit clipped to its missing interval and
UINT64_MAX. At most two reservations are live; when fronts meet they share the
original reservation. A batch never crosses saved coverage or a work boundary.
Within each batch canonical coordinates increase, preserving the job's existing
scalar mapping, kernel and verification rules. A high batch in an orbit stops at
its last variant's lower boundary, so it never mixes variants. Batch order is not
an individual descending lane mapping.

Overflow credits nothing and does not alternate: the same endpoint is retried
with a smaller bound. Accepted low batches remove a prefix; accepted high batches
remove a suffix. Growth is clipped to the remaining work. Each reservation starts
once and finishes once, charging only its own execution and replay time. Pauses
and execution of other reservations are excluded from adaptive timing. Planning
and accepting are separate; output or receipt failure cannot advance coverage.

Restart takes the exact saved complement, starts low again, and may change batch
order, geometry, device or work sizing. The immutable scalar mapping still must
match. Coordinator claim order remains independent of order within a grant.
Native start/summary, checkpoint summary and worker grant-finish report
`batch_order`. No throughput or discovery-probability improvement is claimed.

For an unmapped `[1,18)` range, work width 5 and batch width 3, accepted intervals
are `[1,4)`, `[15,18)`, `[4,6)`, `[13,15)`, `[6,9)`, `[11,13)`, `[9,11)`.
With work width 5 and batch width 2 over `[1,6)`, the fronts share one owner:
`[1,3)`, `[4,6)`, `[3,4)`. With reverse scalar mapping,
these same canonical intervals map through the saved descending progression.

Acceptance requires independent integer sequence/coverage checks, orbit boundary
checks, invalid option rejection, all scalar families and kernels on HIP/CUDA,
exact overflow and restart recovery, and online/offline worker result parity.
Findings and evidence will be recorded in a separate validation document.
