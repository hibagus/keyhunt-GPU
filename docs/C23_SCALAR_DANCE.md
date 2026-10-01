# C23: dance scalar batches

Status: contract defined; implementation and acceptance are in progress.

`--batch-order dance` applies to xpoint, Bitcoin address/HASH160, Ethereum and
Bitcoin vanity, through native searches, checkpoint run, worker run-device and
the Python supervisor. Default remains forward. Checkpoint creation, BSGS and
minikeys reject this scalar-only option, including explicit forward.

Dance repeats low, high, middle after each accepted batch. At invocation start,
let L be the lowest missing coordinate and H the highest missing exclusive end.
The fixed pivot is P = L + floor((H - L) / 2). Subtraction precedes addition to
avoid overflow. Split any missing interval crossing P. Low selects the global
lowest endpoint; high selects the global highest endpoint; middle selects the
lowest remaining endpoint at or above P, falling back to global low when none
remains. The pivot does not move during an invocation, including across saved
holes. Empty coverage requires no pivot or live executor generation.

Selection uses the job's existing receipt coordinates: actual scalars for an
unmapped search and canonical candidate indices for strides, reverse mappings
and six-member orbits. Within every batch these coordinates increase; immutable
`--order forward|reverse` still controls their scalar mapping. Orbit batches stay
within one variant, including a high batch's lower boundary. There is no change
to identities, configuration, schema, receipts, protocol or capabilities.

Each endpoint reserves a contiguous adaptive work unit clipped to its missing
interval, the pivot and UINT64_MAX. At most three reservations are live. Fronts
that meet share the original owner; each owner starts and finishes once. Only
its own execution and replay time contributes to adaptive sizing. Pauses and
execution of other owners are excluded. A map lookup finds the middle endpoint
without scanning the remaining intervals; the pivot adds at most one interval.

Overflow credits nothing and retries the same phase and endpoint with a smaller
batch limit. Planning does not advance coverage or phase; acceptance follows
successful output or durable receipt handling. Batches never cross saved
coverage, work boundaries or the pivot. Restart uses the exact saved complement,
computes its new fixed pivot, and starts low. Batch order, geometry and device
may change; the saved scalar mapping must still match. Coordinator claim order
is independent of selection within a grant. Telemetry reports `batch_order`.

For unmapped [1,102), work and batch widths 17 give P=51 and this sequence:
[1,18), [85,102), [51,68), [18,35), [68,85), [35,51).
The last middle phase falls back to low. This deterministic traversal makes no
throughput or discovery-probability claim.

Acceptance requires independent sequence and ownership checks, including retries,
fragmented gaps, pivot boundaries, changing geometry, wide coordinates, all scalar
mappings and orbit clipping; native HIP/CUDA validation for all scalar families
and kernels; durable recovery and completed retries; online and offline worker
parity; and a verbatim executable public example. Scalar random-window traversal
remains a separate pending C23 slice.
