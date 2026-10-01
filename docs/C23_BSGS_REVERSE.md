# C23: reverse BSGS tile traversal

The selected slice adds `--tile-order forward|reverse` to native BSGS, checkpoint
runs and supervised workers. Forward remains the default. Reverse visits the
highest remaining scalar interval first, then moves downwards. GPU giant/baby
reconstruction still runs forward inside each tile. This option is local to a
grant; coordinator claim policy continues to select blocks independently.

## Coverage and compatibility

BSGS receipts already certify actual scalar intervals after all canonical targets
complete. Tile order is therefore execution policy, not a new candidate mapping.
Job/configuration/table identities, schema 7, result coordinates, protocol and
capability lists stay unchanged. A restart may switch tile order, target subsets,
giant counts or device while preserving exact saved coverage and results.
`checkpoint create` does not accept the option. Non-BSGS runners reject it,
including explicit `forward`. Scalar-search `--order` remains distinct.

For a remaining interval [a,b) and width W=m*max_giants, forward selects
[a,a+min(W,b-a)); reverse selects [b-min(W,b-a),b). Products and endpoints use
checked UInt256 arithmetic. Reverse orders the saved uncovered gaps from highest
to lowest, and selects work units/tiles from each gap's upper end. Tile partitions
can change with direction or launch geometry; their verified union cannot.

Overflow certifies none of an attempt. Target-subset matches may persist before
a tile is complete; partial tiles receive no scalar coverage. Restart replays
all targets for uncovered tiles, with existing durable result deduplication.
Pause drains admitted work, preserves subgroup matches, and flushes only complete
tiles. Prepared targets/table allocations survive grant handoff.

## Acceptance gates

Independent integer tests cover exact tile bounds, high-bit and near-order
intervals, tiny exhaustive partitions and short low-end tails. Native HIP/CUDA
gates compare all target relations with pinned public-key fixtures, exercise
both BSGS grouping kernels, overflow, large origins and all visible ordinals.
Recovery gates cover fragmented complements, lost acknowledgments, killed
restarts, partial-target pause, visibility changes and direction switches.
HTTPS and disconnected-file workers must reconcile identical local/server results
and retain one prepared executor across grants. Record source/binary hashes,
findings, executable examples and raw evidence under docs/.

## Remaining C23 scope

This slice does not implement BSGS both-ends/dance traversal, new random traversal
semantics, or alternative minikey orders. Existing random block-claim policies
remain available and separate from within-block search order. Legacy random
sampling is not advertised as exhaustive. `pub2rmd` remains removed from the
main legacy executable; it is not an unimplemented active search family.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Run both blocks in the same Bash shell. Range and block widths are hexadecimal:
`1:65` contains 100 scalars. With m=17 and two giants per tile, reverse selects
[67,101), [33,67), [1,33) in decimal. Scalar 1 matches the public generator in the
last tile. This example uses public test data.

<!-- bsgs-reverse-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
bsgs_reverse_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-bsgs-reverse.XXXXXX")"
export KEYHUNT_STATE_DIR="$bsgs_reverse_dir/state"
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$bsgs_reverse_dir/points.txt"
"$KEYHUNT_BIN" bsgs-table build --m 17 --output "$bsgs_reverse_dir/babies.khb" \
  > "$bsgs_reverse_dir/table.json"
"$KEYHUNT_BIN" state project-create --name 'Public reverse BSGS example' > "$bsgs_reverse_dir/project.json"
bsgs_reverse_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$bsgs_reverse_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$bsgs_reverse_project" --mode bsgs \
  --range 1:65 --block-width 64 --targets "$bsgs_reverse_dir/points.txt" \
  --table "$bsgs_reverse_dir/babies.khb" > "$bsgs_reverse_dir/job.json"
bsgs_reverse_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$bsgs_reverse_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$bsgs_reverse_project" --job "$bsgs_reverse_job" \
  --owner example --request first > "$bsgs_reverse_dir/grant.json"
bsgs_reverse_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$bsgs_reverse_dir/grant.json")"
```

Select tile order at execution time. Creating the job requires no new option.

<!-- bsgs-reverse-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" bsgs --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:65 --targets "$bsgs_reverse_dir/points.txt" --table "$bsgs_reverse_dir/babies.khb" \
  --giant-batch 2 --tile-order reverse > "$bsgs_reverse_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_reverse_grant" --targets "$bsgs_reverse_dir/points.txt" \
  --table "$bsgs_reverse_dir/babies.khb" --giant-batch 2 --tile-order reverse \
  > "$bsgs_reverse_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_reverse_grant" --targets "$bsgs_reverse_dir/points.txt" \
  --table "$bsgs_reverse_dir/babies.khb" --tile-order forward > "$bsgs_reverse_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$bsgs_reverse_project" --job "$bsgs_reverse_job" \
  > "$bsgs_reverse_dir/results.json"
"$KEYHUNT_BIN" state check > "$bsgs_reverse_dir/check.json"
```

`results.json` contains scalar 1 once. `retry.ndjson` reports 100 resumed scalars
and zero batches. Changing tile order on a partially completed grant likewise
searches only the complement of saved coverage, with any new tile geometry.

## Initial findings

The complete CPU suite passed 114 gates; three focused ASan/UBSan gates passed.
The independent integer oracle checks 5,535 bounds and reconstruction cases.
MI300X native search, interrupted restart, pause/backup/restore, visibility change,
HTTPS and disconnected file execution passed. A recovered grant with three saved
islands visits all four missing gaps without recomputing the islands; old results
remain on the coordinator and deduplicate with a retried upload.

The first pause invocation lacked the existing pinned Python Keccak environment,
which that shared harness imports for its other mode families. Setting
`KEYHUNT_KECCAK_ORACLE_ROOT=/var/tmp/keyhunt-c23-ethereum-oracle` resolved the
collection failure; no production code change was needed. Acceptance records
will distinguish host checks, device cases and the limits of this slice.
