# C23: exact minikey dance traversal

This slice adds execution-only `--ordinal-order dance` to native minikey
searches, checkpoint runs and supervised workers, for both 22- and 30-character
formats and all supported encodings. Forward remains the default.

## Selection contract

At invocation start, let L be the lowest missing ordinal and H the highest
exclusive endpoint. Compute P = L + floor((H-L)/2), and split any missing
interval crossing P once. Cycle through these selections per accepted batch:

1. Global lowest missing endpoint, forward.
2. Global highest missing endpoint, reverse.
3. Lowest missing endpoint at or above P, forward; fall back to global low
   when no missing ordinal remains at or above P.

The pivot stays fixed for the invocation, including across saved holes. Batches
and work reservations cannot cross it. A fixed pivot adds at most one gap and
allows at most three active reservations, independently of domain size. The
planner uses indexed interval lookup rather than scanning every saved gap.
This deterministic order provides exact coverage; it does not emulate legacy
random sampling.

Planning reserves work but consumes no coverage. Overflow retries the same
endpoint with a smaller batch and preserves the phase. Receipt validation,
CPU verification and durable accounting precede advancement. Interleaved work
units retain their original bounds and record only their own execution time.
When fronts meet, they share existing work ownership.

## Recovery and compatibility

Restart rebuilds the missing intervals, recomputes the pivot and starts low.
It may change among forward, reverse, both-ends and dance, or change batch/work
geometry and device. Receipts remain canonical ascending half-open ordinal
intervals; result ordinals and target relations retain their existing meaning.
Prepared targets survive direction and grant changes.

Job/configuration/target identities, schema 7, protocol, capabilities and
`minikey-ordinal-v1` stay unchanged. Existing `minikeys-v1` workers can resume
forward. Scalar `--order`, BSGS `--tile-order` and block claim policy remain
separate. Creation and non-minikey runners reject ordinal-order overrides.
Scalar strides, orbit expansion, stepped/GLV kernels and other key lengths
remain unsupported for minikeys.

## Acceptance plan

Check independent sequences for both lengths, all four orders, fragmented
complements, tiny and full domains, limb/base-58 carries, pivot gaps, changing
geometry, overflow retries and bounded work ownership. Validate native HIP and
CUDA searches against the independent public-key oracle on every visible GPU.
Exercise lost acknowledgments, killed policy-switch restarts, pause/restore,
old-worker capabilities and HTTPS/disconnected file transports. Record source
and binary identities, executable examples and raw evidence under docs/.

## Remaining C23 scope

Random traversal semantics remain pending. This policy makes no throughput
or fleet-scaling claim.
