# C23: exact reverse minikey traversal

This slice adds execution-only `--ordinal-order forward|reverse` to native
minikey search, checkpoint runs and supervised workers. Both 22- and
30-character formats and all existing encoding selections are supported.
Forward remains the default. This option is separate from immutable scalar
`--order` and from BSGS `--tile-order`.

## Mapping and coverage

Direction changes scheduling, not the canonical ordinal-to-text mapping. Reverse
selects the highest missing interval, reserves work from its upper endpoint and
submits bounded batches downward. For a batch [L,H), logical lane i represents
ordinal H-1-i. GPU lanes execute concurrently; direction describes this mapping
and the submission sequence, not a serial wall-clock execution guarantee.

Every ordinal is tested, including candidates rejected by their check byte. CPU
verification derives the ordinal from the batch and lane, reconstructs the text,
and validates the private scalar and encoding/HASH160 relation. Match and receipt
coordinates remain actual one-based minikey ordinals (`minikey-ordinal-v1`).

An internal reverse-minikey execution tag distinguishes lane interpretation and
is included in completion identity checks. It is not serialized as a new job
configuration. A scalar mapping cannot be attached to either minikey algorithm.
The GPU executor reads direction from each submitted batch, allowing preparation
to survive grant boundaries without retaining stale direction state.

Overflow invalidates the whole attempt. Replay shrinks from the same high
endpoint, then advances only after verified completion. Adaptive work units are
contiguous and cannot cross saved coverage. Pause drains the admitted batch;
restart reconstructs the exact complement and may change direction or geometry.

## Compatibility and scope

Job/configuration/target identity, schema 7, coordinator protocol, capability
lists and result coordinates remain unchanged. Existing `minikeys-v1` workers
can execute the same jobs forward, including after recovery from a reverse
owner. Block claim policy still selects grants independently of ordinal order.
`checkpoint create` and non-minikey runners reject `--ordinal-order`, including
explicit forward. Minikey scalar strides, scalar `--order`, orbit expansion,
stepped/GLV kernels, other lengths and random traversal remain unsupported.

## Acceptance plan

Use independent integer sequence and candidate/hash/public-key oracles across
both lengths, base-58 and limb boundaries, domain endpoints, changing batches,
all visible devices, forced overflow and both encodings. Check batch identities,
invalid options and wrong-mode rejection before coverage can be written.

Exercise acknowledgment loss, killed restarts in both directions, fixed/adaptive
work, pause/restore/visibility changes, fragmented grant recovery, old-worker
capability compatibility, byte-identical upload retries, HTTPS and disconnected
file workers. Keep executable examples and raw evidence under docs/.

## Remaining C23 scope

Additional random traversal semantics and other minikey orders remain pending.
This slice makes no throughput or fleet-scaling claim.
