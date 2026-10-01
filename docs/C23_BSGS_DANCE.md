# C23: exact BSGS dance traversal

This slice adds execution-only `--tile-order dance` to native BSGS, checkpoint
runs and supervised workers. It cycles low, high, middle per completed tile.
The middle front starts at a fixed midpoint and advances upward. This is an
exact deterministic policy, distinct from legacy `-B dance`, whose random
interior samples can overlap and do not remove that interior from pending work.

## Selection and bounded state

For the sorted nonempty missing intervals at invocation start, let L be the
lowest begin and H the highest exclusive end. Set P = L + floor((H-L)/2).
Split any missing interval that crosses P once. Tiles and accounting work units
cannot cross P or previously accepted coverage. Each cycle selects:

1. The lowest remaining endpoint, searching forward.
2. The highest remaining endpoint, searching backward by tiles.
3. The lowest remaining endpoint at or above P, searching forward; if none
   remains, use the global lowest endpoint.

The midpoint is fixed for this invocation, including across gaps. It is not
recomputed per tile or based on uncovered rank. A moving midpoint would create
many interior holes on large ranges. A fixed pivot adds at most one missing
interval and three active contiguous work units. Planner memory is bounded by
the input gap count plus four entries, independent of the number of tiles.
The interval index supports endpoint selection and middle lookup without
scanning all saved gaps. Giant/baby arithmetic inside each tile stays forward.

Adaptive sizing determines a new work unit's span when it starts. Units may
interleave; at most three remain active. Each unit's own admitted execution time
updates sizing when it completes. Target subsets and overflow replays finish
the same tile before the phase advances. Partial matches are durable but an
incomplete target sweep certifies no scalar coverage.

## Restart and compatibility

Selection state is process-local. Restart recomputes L, H and P from the saved
complement and starts low. Geometry, device, target subsets and tile policy may
change. Submission order need not match an uninterrupted run; the scalar union
and deduplicated verified results must match. Selection is within a single
grant, independently of coordinator block claim policy.

Job/configuration/table identity, schema 7, receipts, capabilities and scalar
result coordinates stay unchanged. The default remains forward; forward,
reverse and both-ends retain their existing sequences. `checkpoint create` and
non-BSGS runners reject tile-order overrides, including explicit forward.

## Acceptance plan

Compare full tile/work sequences with an independent integer model on tiny
ranges, fragmented gaps, changing work sizes, huge products and curve-order
boundaries. Verify nonoverlap, exact completion, balanced unit ownership and
bounded fragmentation. Retain all existing traversal oracle cases.

Exercise subgroup overflow and lost acknowledgments, adaptive units, pause,
fragmented grant transfer and lost upload replies. Check native HIP/CUDA parity
against the pinned public-key oracle, both kernels, all visible ordinals,
killed policy-switch restarts and online/disconnected workers. Record build
identities, raw logs and machine-readable acceptance evidence in docs/.

## Remaining C23 scope

Additional random traversal semantics and alternative minikey orders remain
pending. This policy does not emulate legacy random sampling or claim a kernel
throughput improvement.
