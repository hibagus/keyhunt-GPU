# C23: exact reverse scalar traversal

Status: complete on the recorded MI300X/H200 stacks; see [acceptance and evidence](C23_REVERSE_VALIDATION.md).
This slice covers xpoint, Bitcoin
HASH160/P2PKH, Ethereum and Bitcoin vanity, with unit or positive nonunit strides.
BSGS/minikey traversal and endomorphism remain separate mappings.

## Coverage and ordering

`--range A:B --stride S --order reverse` visits the same finite progression as
forward search, in reverse order. With `N=1+floor((B-A-1)/S)`, candidate index
`j` in `[1,N+1)` maps to `A+(N-j)*S`. The first scalar is the last **on-lattice**
scalar below B, which is not necessarily B-1. Bounds and stride are hexadecimal;
`1<=A<B<=n` and `1<=S<n` remain required. No subtraction wraps modulo n.

For example, `--range 65:80 --stride 7` has four candidates. The exclusive
end `0x80` leaves a short tail after `0x7a`; reversing does not change that lattice.

| Candidate index | Forward scalar | Reverse scalar |
| --- | --- | --- |
| 1 | `0x65` | `0x7a` |
| 2 | `0x6c` | `0x73` |
| 3 | `0x73` | `0x6c` |
| 4 | `0x7a` | `0x65` |

Reverse work always uses one-based candidate-index coverage, even at stride one.
Blocks, batch intervals, receipts, saved complements and result coordinates use
those indices. Summaries count candidates, and public results show actual
`scalar` and `candidate_index` separately with
`coordinate_space:"scalar-reverse-index-v1"`. Generic state/protocol interval
records retain their existing shape and inherit meaning from the job binding.

`--order forward` is the default and preserves existing version-1 unit-stride
and version-2 nonunit-stride identities. Reverse order uses configuration version
3 with the same 146-byte layout as the strided binding: original scalar bounds
and positive stride follow the header. Version 3 implies reverse order and is
valid at stride one. The exact candidate root and all mapping values are checked.
A different order creates a different job; committed forward coverage is never
silently reinterpreted as reverse coverage. Schema 7 and receipt bytes stay fixed.

## Execution and recovery

The direct kernel uses checked multiply-subtract. The stepped kernel starts at
the batch's first mapped scalar and advances by `-SG`; cached point offsets use
`(n-S)*2^bit mod n`. Only point arithmetic is modular. Prepared executors bind
both stride and direction, reject mismatched work and refresh each batch seed.
Every returned relation is checked against the mapped scalar on the CPU.

Existing whole-attempt overflow rejection and bounded replay apply to candidate
indices. Checkpoint creation binds order, while runs infer it from saved state;
an explicit order must agree. Pause, restart, changed launch geometry/device,
backup and ownership fences retain the original mapping.

Workers must advertise an eighth capability, `scalar-reverse-v1`, before a
coordinator allocates, renews, updates or returns cached replies for reverse jobs.
Older capability sets retain their supported forward jobs. Fresh self-tests run
both reverse kernels on the owned device, and prepared allocations survive grant
handoff. HTTPS and disconnected courier transport carry the full binding.

## Acceptance gates

Independent integer and pinned public-key oracles must check mapping/inverse,
non-divisible tails, wide subtraction/borrows, curve-order boundaries, skipped
targets, both kernels and all four families. Durable tests must cover mismatched
order, overflow, kill/restart, pause, changed visibility and exact local/server
relations. CPU/sanitizer and real HIP/CUDA validation are required before marking
this slice complete. Findings, source phases, binary hashes and raw evidence
are recorded in the acceptance artifacts under docs/. This slice makes no throughput or scaling claim.

## GPU implementation evidence

The initial HIP gate passed 112 independent reverse CLI cases across both kernels
and all eight MI300X ordinals, including unit strides, non-divisible tails, wide
origins/steps, curve-order boundaries, no-hit and overlapping targets, overflow
replay and maximum-size batches. Portable/native subtraction passed 2,509 integer
oracle vectors. Executor checks reject direction/stride mismatches and accept a
changed origin with a fresh seed. The 104-case forward-stride regression also
passed. CUDA and final durable/worker gates are recorded in the acceptance evidence.

## Durable implementation evidence

Version 3 now binds reverse order for both unit and nonunit strides. Creation
persists the original scalar bounds; runs infer order and stride, or reject an
explicit mismatch before execution. Results and summaries label reverse indices
and report actual scalars separately. BSGS/minikey jobs reject scalar order flags.
The 12-test CPU storage gate passed, covering malformed bindings, changed order,
lost acknowledgements, dense overflow/replay, pause, backup and completed retries.
Real HIP/CUDA checkpoint and changed-visibility pause gates also passed.

## Coordinator implementation evidence

The coordinator now fences reverse jobs with `scalar-reverse-v1` before cached
replies or durable mutations. The seventh-capability forward worker remains
supported. Persistent GPU owners bind stride and direction from validated work,
and fresh per-device self-tests exercise both orders and both kernels.
Seven CPU coordinator gates passed, including four-family reverse import,
capability downgrades, exact public results and lost-upload acknowledgement replay.
The live transport fixture passed on HIP/CUDA. It covers unit-stride reverse over
HTTPS and wide-stride reverse with the server stopped during courier-file execution.
