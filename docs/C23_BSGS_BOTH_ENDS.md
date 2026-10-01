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

## Findings

The complete CPU suite passed 120 gates. Five focused ASan/UBSan gates passed.
The independent sequence oracle checks 3,164 cases; the existing 5,535-case tile
bounds/reconstruction oracle remains a regression gate. MI300X passed 16 selected
gates, including forward/reverse regressions, both kernels, killed restarts,
pause/visibility changes and both worker transports.

The first independent sequence model failed to exclude an opposite active
reservation and to clip to the remaining middle span. For a three-scalar interval
and two-scalar tiles, the correct second tile contains only the last scalar.
Adding those missing bounds fixed the reference model; production already returned
the exact clipped sequence. The initial failure and passing suite are retained.

Work units remain contiguous accounting regions, but their tiles may interleave.
The adaptive estimate uses each unit's own active time; its wall completion can
also include tiles from the opposite unit. Pause still drains only admitted GPU
work. At restart, begin low on remaining coverage instead of persisting a fragile
tile counter or random state. No new kernel or throughput claim is introduced.

## Remaining C23 scope

Dance, additional random traversal semantics and alternative minikey orders remain
pending. Existing random block claims stay available independently of tile order.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Run both blocks in the same Bash shell. The hexadecimal range `1:65` contains 100
scalars. With m=17 and two giants per tile, both-ends visits [1,35), [67,101), then
[35,67) in decimal. Scalar 1 matches the public generator in the first tile.

<!-- bsgs-both-ends-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
bsgs_both_ends_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-bsgs-both-ends.XXXXXX")"
export KEYHUNT_STATE_DIR="$bsgs_both_ends_dir/state"
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$bsgs_both_ends_dir/points.txt"
"$KEYHUNT_BIN" bsgs-table build --m 17 --output "$bsgs_both_ends_dir/babies.khb" \
  > "$bsgs_both_ends_dir/table.json"
"$KEYHUNT_BIN" state project-create --name 'Public both-ends BSGS example' > "$bsgs_both_ends_dir/project.json"
bsgs_both_ends_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$bsgs_both_ends_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$bsgs_both_ends_project" --mode bsgs \
  --range 1:65 --block-width 64 --targets "$bsgs_both_ends_dir/points.txt" \
  --table "$bsgs_both_ends_dir/babies.khb" > "$bsgs_both_ends_dir/job.json"
bsgs_both_ends_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$bsgs_both_ends_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$bsgs_both_ends_project" --job "$bsgs_both_ends_job" \
  --owner example --request first > "$bsgs_both_ends_dir/grant.json"
bsgs_both_ends_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$bsgs_both_ends_dir/grant.json")"
```

<!-- bsgs-both-ends-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" bsgs --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:65 --targets "$bsgs_both_ends_dir/points.txt" --table "$bsgs_both_ends_dir/babies.khb" \
  --giant-batch 2 --tile-order both-ends > "$bsgs_both_ends_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_both_ends_grant" --targets "$bsgs_both_ends_dir/points.txt" \
  --table "$bsgs_both_ends_dir/babies.khb" --giant-batch 2 --tile-order both-ends \
  > "$bsgs_both_ends_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_both_ends_grant" --targets "$bsgs_both_ends_dir/points.txt" \
  --table "$bsgs_both_ends_dir/babies.khb" --tile-order forward > "$bsgs_both_ends_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$bsgs_both_ends_project" --job "$bsgs_both_ends_job" \
  > "$bsgs_both_ends_dir/results.json"
"$KEYHUNT_BIN" state check > "$bsgs_both_ends_dir/check.json"
```

`results.json` contains scalar 1 once. The final forward retry reports 100 resumed
scalars and zero batches, using the same job and grant.
