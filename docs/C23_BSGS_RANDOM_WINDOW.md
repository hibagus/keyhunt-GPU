# C23: exact seeded BSGS random windows

This slice adds execution-only `--tile-order random-window` to native BSGS,
checkpoint runs and supervised workers. `--tile-seed HEX` is a 256-bit seed
(default zero); `--tile-window 1..256` bounds the number of shuffled tiles
(default 64). Start/summary and worker completion records expose both settings.
The default tile order remains forward.

## Selection and bounded state

Build a window from the next W tiles at the lowest missing endpoints, clipping
every tile at saved gaps. Tile width is m times the giant-batch limit. Shuffle
those tiles, exhaust the entire window, then build the next window. A window may
span different missing intervals but no tile or accounting unit crosses a saved
hole. Window size one is sequential. The policy is local randomization within
ascending windows, not globally uniform random selection or legacy sampling
with replacement. Coordinator block claim policy remains independent.

At window creation, sample the current adaptive work span. Round its tile count
down, with a minimum of one tile, and partition the window into contiguous work
units of at most that many tiles. Short gap/window tails clip units. Shuffle
tiles across those units, announcing each unit at its first tile and completing
it at its last tile. New sizing observations apply to the next window. Each
unit records only its own execution time. This makes random selection meaningful
even with fixed one-tile work units and bounds simultaneous ownership by W.

Only W tile records and bounded ownership metadata are needed in addition to the
input gaps. No new window opens until every target subgroup of the previous
window's last tile is verified. At most W additional receipt fragments can be
introduced before completing the current window; huge domains are never
materialized. GPU arithmetic inside each tile stays forward.

## Reproducible shuffle

Use descending Fisher-Yates. To select j in [0,i], hash the bytes
`khbsgs-window-v1\0` followed by the 32-byte big-endian seed and 32-byte big-endian
counter using SHA-256. The counter starts at zero per invocation and advances
for every draw. Mask the first digest byte to the smallest all-ones mask covering
i; reject values greater than i. Fail closed after 1,024 rejected draws or on
counter overflow. This avoids modulo bias and library-dependent shuffle rules.
The seed is an ordering control, not a security key or a throughput guarantee.

## Recovery and compatibility

Overflow and partial target groups retain the same tile and consume no new
random draw. Verified subgroup matches may be saved early, but a tile certifies
coverage only after all targets finish. Restart rebuilds the missing complement
and resets the stream; seed, window, geometry and policy may change. The same
seed reproduces a sequence only with the same initial gaps, geometry and work
sizes. Exact scalar coverage and deduplicated results define recovery.

No job/configuration/table identity, schema 7, protocol or capability changes.
Existing BSGS workers can resume forward. Non-BSGS runners and checkpoint
creation reject the new options; seed/window overrides require random-window.
Scalar `--order` and minikey `--ordinal-order` remain separate.

## Acceptance plan and remaining scope

Check independent integer/hashlib sequences, seed endpoints, rejection draws,
window bounds, all earlier orders, fragmented recovery, changing work sizes,
subgroup overflow, lost acknowledgments and killed policy-switch restarts.
Validate both GPU kernels, every visible device, pause/restore and supervised
HTTPS/disconnected file transports on HIP and CUDA. Preserve raw evidence and
executable examples. Other families' random traversal remains pending in C23.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Run both blocks in the same Bash shell. The hexadecimal range `1:65` contains 100
scalars. With m=17, two giants per tile, seed `2a` and window size four,
the three tiles are shuffled reproducibly. The full range is covered exactly;
scalar 1 matches the public generator once.

<!-- bsgs-random-window-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
bsgs_random_window_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-bsgs-random-window.XXXXXX")"
export KEYHUNT_STATE_DIR="$bsgs_random_window_dir/state"
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$bsgs_random_window_dir/points.txt"
"$KEYHUNT_BIN" bsgs-table build --m 17 --output "$bsgs_random_window_dir/babies.khb" \
  > "$bsgs_random_window_dir/table.json"
"$KEYHUNT_BIN" state project-create --name 'Public random-window BSGS example' > "$bsgs_random_window_dir/project.json"
bsgs_random_window_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$bsgs_random_window_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$bsgs_random_window_project" --mode bsgs \
  --range 1:65 --block-width 64 --targets "$bsgs_random_window_dir/points.txt" \
  --table "$bsgs_random_window_dir/babies.khb" > "$bsgs_random_window_dir/job.json"
bsgs_random_window_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$bsgs_random_window_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$bsgs_random_window_project" --job "$bsgs_random_window_job" \
  --owner example --request first > "$bsgs_random_window_dir/grant.json"
bsgs_random_window_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$bsgs_random_window_dir/grant.json")"
```

<!-- bsgs-random-window-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" bsgs --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:65 --targets "$bsgs_random_window_dir/points.txt" --table "$bsgs_random_window_dir/babies.khb" \
  --giant-batch 2 --tile-order random-window --tile-seed 2a --tile-window 4 > "$bsgs_random_window_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_random_window_grant" --targets "$bsgs_random_window_dir/points.txt" \
  --table "$bsgs_random_window_dir/babies.khb" --giant-batch 2 --tile-order random-window --tile-seed 2a --tile-window 4 \
  > "$bsgs_random_window_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$bsgs_random_window_grant" --targets "$bsgs_random_window_dir/points.txt" \
  --table "$bsgs_random_window_dir/babies.khb" --tile-order forward > "$bsgs_random_window_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$bsgs_random_window_project" --job "$bsgs_random_window_job" \
  > "$bsgs_random_window_dir/results.json"
"$KEYHUNT_BIN" state check > "$bsgs_random_window_dir/check.json"
```

`results.json` contains scalar 1 once. The final forward retry reports 100 resumed
scalars and zero batches, using the same job and grant.
