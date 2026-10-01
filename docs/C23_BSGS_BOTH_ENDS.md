# C23: exact BSGS both-ends traversal

The selected slice adds execution-only `--tile-order both-ends` to native BSGS,
checkpoint runs and supervised workers. Each invocation starts at the lowest
uncovered tile, then alternates highest, lowest, highest until the grant is
complete. This deterministic policy is distinct from legacy `-B both`, which
randomly chooses an end. Dance and other random semantics remain pending.

## Coverage and restart contract

Selection always uses the global lowest/highest remaining scalar endpoint,
including when saved coverage leaves several gaps. A tile cannot cross accepted
coverage or an active work-unit boundary. Giant/baby arithmetic stays forward
inside each tile. Target subsets and overflow retries must finish the same tile
before the owner selects the opposite end.

Adaptive worker sizing still controls contiguous work units. At most two units
can be partially processed at opposite ends; when they meet, both ends may draw
from the same unit. Completed-unit active time updates the next size estimate.
A unit can therefore contain nonconsecutive tile submissions. This preserves
actual tile alternation even when one work unit grows to cover the remaining
range. Planner memory is bounded by the saved gaps plus two active units.

The planner is process-local, not durable state. A restart begins low again on
the exact complement of committed scalar intervals. It may change tile order,
geometry, device or target subsets. This can change submission order but cannot
change the certified scalar union. Partial-target matches remain durable, while
an incomplete tile receives no scalar coverage and replays with deduplication.

Job/configuration/table identity, schema 7, receipts, capability lists and result
coordinates remain unchanged. Block claim policy remains separate. `checkpoint
create` and non-BSGS runners reject tile-order overrides, including forward.
The existing forward/reverse options and default forward retain their meanings.

## Acceptance plan

Use an independent integer sequence oracle over tiny exhaustive walks, changing
work sizes, fragmented gaps, wide products and near-order bounds. Test that each
selected tile uses the expected global end, no scalar repeats, tails meet exactly,
and work-unit starts/completions balance. Include invalid options and malformed
gap lists. Verify native HIP/CUDA public-key parity on both kernels and all eight
visible ordinals; test overflow and maximum-giant tails.

Exercise killed restarts into/out of both-ends, subgroup acknowledgment loss,
pause/restore and changed visibility. Recover fragmented coordinator grants using
the existing protocol, preserve prior-owner matches, retry lost uploads, and run
HTTPS/disconnected-file workers across two grants with one prepared executor.
Record comments, decisions, executable examples and acceptance evidence in docs/.
