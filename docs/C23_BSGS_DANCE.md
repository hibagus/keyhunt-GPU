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

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Run both blocks in the same Bash shell. The hexadecimal range `1:65` contains 100
scalars. The fixed midpoint is 51. With m=17 and two giants per tile, dance
visits [1,35), [67,101), [51,67), then [35,51) in decimal. Scalar 1 matches the public generator in the first tile.

<!-- bsgs-dance-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
bsgs_dance_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-bsgs-dance.XXXXXX")"
export KEYHUNT_STATE_DIR="$bsgs_dance_dir/state"
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$bsgs_dance_dir/points.txt"
"$KEYHUNT_BIN" bsgs-table build --m 17 --output "$bsgs_dance_dir/babies.khb" \
  > "$bsgs_dance_dir/table.json"
"$KEYHUNT_BIN" state project-create --name 'Public dance BSGS example' > "$bsgs_dance_dir/project.json"
bsgs_dance_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$bsgs_dance_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$bsgs_dance_project" --mode bsgs \
  --range 1:65 --block-width 64 --targets "$bsgs_dance_dir/points.txt" \
  --table "$bsgs_dance_dir/babies.khb" > "$bsgs_dance_dir/job.json"
bsgs_dance_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$bsgs_dance_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$bsgs_dance_project" --job "$bsgs_dance_job" \
  --owner example --request first > "$bsgs_dance_dir/grant.json"
bsgs_dance_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$bsgs_dance_dir/grant.json")"
```

<!-- bsgs-dance-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" bsgs --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:65 --targets "$bsgs_dance_dir/points.txt" --table "$bsgs_dance_dir/babies.khb" \
  --giant-batch 2 --tile-order dance > "$bsgs_dance_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_dance_grant" --targets "$bsgs_dance_dir/points.txt" \
  --table "$bsgs_dance_dir/babies.khb" --giant-batch 2 --tile-order dance \
  > "$bsgs_dance_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_dance_grant" --targets "$bsgs_dance_dir/points.txt" \
  --table "$bsgs_dance_dir/babies.khb" --tile-order forward > "$bsgs_dance_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$bsgs_dance_project" --job "$bsgs_dance_job" \
  > "$bsgs_dance_dir/results.json"
"$KEYHUNT_BIN" state check > "$bsgs_dance_dir/check.json"
```

`results.json` contains scalar 1 once. The final forward retry reports 100 resumed
scalars and zero batches, using the same job and grant.
