# C23: exact both-ends minikey traversal

This slice adds execution-only `--ordinal-order both-ends` to native minikey
search, checkpoint runs and supervised workers. It covers the 22- and
30-character domains and all supported public-key encodings. Forward remains
the default; forward and reverse retain their existing sequences.

## Selection and recovery contract

Start at the lowest uncovered ordinal, then alternate low and high after each
successfully verified batch. A low batch maps logical lanes upward; a high batch
maps them downward. Selection uses the global endpoints of the missing interval
set, including fragmented recovered grants. GPU lanes still run concurrently.

Overflow advances neither coverage nor the alternating phase. Replay shrinks
from the same endpoint. Output and receipt validation precede advancement.
Receipts remain ascending half-open intervals of canonical minikey ordinals;
results retain their actual ordinal and target relation.

Adaptive work reservations are contiguous and cannot overlap or cross saved
coverage. Batch alternation continues inside large reservations. When the fronts
meet, they share the existing reservation and clip to its remaining span. Each
reservation records only its own active execution time, including replay; pauses
and time spent on the opposite front are excluded. At most two reservations are
active, independent of the size of the ordinal domain.

Restart reconstructs missing intervals from durable receipts and starts low.
It may switch among forward, reverse and both-ends, change batch geometry, or
move to a compatible worker. Alternating phase is not persisted: exact coverage
is the recovery contract. Prepared GPU targets survive direction and grant
changes, with direction taken from each submitted batch.

## Compatibility and scope

Job/configuration/target identities, schema 7, protocol, capability lists and
`minikey-ordinal-v1` coordinates remain unchanged. Existing `minikeys-v1` workers
can recover these jobs forward. Block claim policy remains independent of the
within-grant ordinal order. Scalar `--order` and BSGS `--tile-order` are separate.
`checkpoint create` and non-minikey runners reject `--ordinal-order`, even when
forward is explicit. Scalar strides, orbit expansion, stepped/GLV kernels and
other lengths remain unsupported for minikeys.

## Acceptance plan

Check independent candidate/sequence oracles for both lengths, tiny and huge
intervals, work reservations meeting at the middle, changing batch/work sizes,
base-58/limb carries and exact endpoints. Preserve forward/reverse regression
coverage, overflow replay, all encodings and all visible device ordinals.

Exercise fragmented receipt recovery, acknowledgment loss, killed restarts in
and out of both-ends, adaptive work, pause/restore/visibility changes, old-worker
compatibility and byte-identical upload retry. Check supervised HTTPS and
manual disconnected file transports on HIP and CUDA. Record executable examples,
source/binary identities and raw evidence under docs/.

## Remaining C23 scope

Additional random traversal semantics and other minikey orders remain pending.
This policy makes no throughput or fleet-scaling claim.
