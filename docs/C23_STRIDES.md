# C23: exact positive scalar strides

Status: implementation in progress. The selected scope is xpoint, Bitcoin
HASH160/P2PKH, Ethereum and Bitcoin vanity, with HIP/CUDA execution, durable
checkpoints and online/offline owners. BSGS, minikey enumeration, endomorphism
and alternative search orders remain separate mappings.

## Candidate and coverage contract

`--range A:B --stride S` visits exactly `A + i*S < B`, starting with `i=0`.
Range endpoints, block widths and stride are hexadecimal; batch sizes are decimal.
The range must satisfy `1 <= A < B <= n`; the stride must satisfy `1 <= S < n`.
There is no modular wrap, endpoint swapping, random sampling or skipped prefix.
The legacy `-I` implementation remains unchanged: its batch overlap and tail
behavior are characterized in [the CPU baseline](CPU_BASELINE.md).

Stride 1 preserves the existing consecutive-scalar interface, identities and
version-1 bindings. Stride greater than 1 has a one-based candidate index `j`:

- `scalar(j) = A + (j-1)*S`;
- `N = 1 + floor((B-A-1)/S)`;
- coverage domain `[1,N+1)`.

Blocks, grants, work units, batches and saved complements operate in candidate
indices for strided jobs. Block width therefore counts candidates, not the
numeric distance between private scalars. This permits exact replay even with
wide strides and short final blocks. Public records identify
`coordinate_space:"scalar-stride-index-v1"`, show `candidate_index` separately
from the actual `scalar`, and use `computed_candidates`/`resumed_candidates`.
Historical receipt/SQL fields named `scalar` contain the candidate index for
these bindings. A relation is `(candidate index, canonical target)`.

## Binding and compatibility

A strided binding uses configuration version 2: the existing 50-byte header
with the same family mode and zero table fields, followed by 32-byte big-endian
A, B and S, for 146 bytes total. The stored manifest root must equal `[1,N+1)`.
All three scalar mapping values participate in the algorithm digest. Target
formats and digests retain their existing family semantics. Version 2 rejects
stride 1, unsupported modes, nonzero table fields and inconsistent roots.
Schema 7 and published migrations are unchanged.

Work algorithms explicitly distinguish strided families and retain a validated
mapping in execution identity. Device executors bind their step independently
and reject mismatched batches. CPU verification maps candidate indices back to
scalars before checking complete public-key relations. Old workers must advertise
`scalar-stride-v1` before the coordinator can allocate, renew or accept updates
or cached receipts for a version-2 job. Previous capability lists remain valid
for their supported version-1 jobs.

## GPU execution and recovery

The direct kernel computes the checked affine scalar mapping for each local
candidate index. The stepped kernel starts at the first mapped scalar's point
and uses cached powers of `S*G`, stepping by `S*G`. Point-cache arithmetic is
modulo the group order; candidate enumeration never wraps. The unchanged
stride-one specialization retains its existing arithmetic path.

Existing per-family output bounds still apply. Overflow discards the entire
attempt and credits no candidate indices; replay shrinks the exact same interval.
Targets, stride and device allocations survive supervised grant handoff. Pause,
process death, ownership fences, offline leases and acknowledgment retries use
the existing journal protocol with the explicit version-2 coordinate mapping.
No BSGS table or calibrated block-width claim applies to this slice.

## Acceptance gates

Independent Python integers and pinned libsecp256k1 cover tiny exhaustive
progressions, non-divisible tails, skipped targets, high-bit origins and strides,
curve-order endpoints and invalid mappings. Both kernels and all four families
must agree on HIP and CUDA. Storage and coordinator gates cover immutable stride
identity, overflow, restart, pause, old-worker rejection and authenticated/offline
result reconciliation. Executable examples and source/binary-bound evidence are
recorded under `docs/` before acceptance.

## GPU implementation and first validation

Direct scalar kernels use a separately compiled checked multiply-add path when
stride exceeds one. Stepped kernels cache `(S * 2^bit mod n)G` for the twenty
kernel-local offset bits, then advance by `SG`. Every batch receives a fresh
CPU-derived seed at its mapped private scalar. The prepared executor rejects a
batch whose stride differs from its immutable options.

Initial MI300X validation passed the portable/native arithmetic oracle, all four
existing executor ownership/fault suites and 104 independent CLI cases across
both kernels and all eight visible GPUs. Cases include 128/192-bit arithmetic,
order boundaries, off-lattice targets, overlapping vanity relations, bounded
buffer overflow/replay and the maximum 1,048,576-candidate batch. Explicit stride
one retains the existing coordinate space and target relations. These checks are
correctness evidence, not throughput or multi-GPU scaling measurements.

## Durable mapping

The checkpoint binding now uses the specified 146-byte configuration for nonunit
strides. Decoding checks canonical targets, version, length, zero table fields,
positive nonunit stride and exact candidate-index root before accepting the job.
The unchanged receipt format stores candidate indices in its historical `scalar`
field; both executor and journal verification map that index to a private scalar.
Public result records show both `candidate_index` and the actual `scalar`.

`checkpoint create --stride HEX` binds the progression. `checkpoint run` recovers
it automatically; an explicit stride must match. Candidate counts are reported as
`computed_candidates` and `resumed_candidates`. Block widths count candidates.
BSGS and minikey jobs reject the scalar-stride option. Unit stride keeps the old
50-byte configuration and job identity, with no schema migration.

CPU tests cover all four families, malformed configurations with recalculated
digests, mismatch rejection, dense overflow, lost commit acknowledgements, pause,
backup and completed retry. HIP integration verifies 24 direct/stepped durable
cases plus four killed-process restarts against independent target relations.
