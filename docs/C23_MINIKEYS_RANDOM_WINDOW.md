# C23: exact seeded minikey random windows

This slice adds execution-only `--ordinal-order random-window` to native
minikey search, checkpoint runs and supervised workers, for 22/30-character
formats and all supported encodings. `--ordinal-seed HEX` is a 256-bit ordering
seed (default zero); `--ordinal-window 1..256` bounds tiles per window (default
64). Both overrides require random-window. Forward remains the default.

## Selection and overflow

At window creation, sample the current batch limit B. Form the next W ordinal
tiles of at most B candidates from the lowest missing endpoints, clipping at
saved gaps. A window can span several gaps, but no tile or work unit crosses a
hole. Shuffle these tiles and exhaust the window before constructing the next.
This randomizes locally within ascending windows; it is not global uniform
selection or legacy sampling with replacement. Window size one is sequential.

Within a selected tile, batches visit increasing ordinals. Overflow replans at
the same unaccepted ordinal with a smaller batch and consumes no new random
draw. Successful sub-batches consume their exact interval; the tile's remainder
is exhausted before selecting another shuffled tile. A growing batch limit is
clipped at that tile's endpoint. Receipt validation, CPU verification and
accounting precede acceptance. Later windows sample the updated batch limit.

Work units are partitioned before shuffling. Their tile count is the current
work span divided by B, rounded down with a minimum of one, capped by W and the
64-bit work-unit bound. Gap/window tails clip units. Each owner starts on its
first submitted tile and finishes on its last accepted sub-batch. Execution and
replay time belong only to that owner, excluding pauses and other owners. New
adaptive observations affect the next window. State is bounded by W tile records
and W active owners beyond the input gaps; at most W additional accepted
fragments can occur while completing a window. Huge domains are never expanded.

## Reproducible order

Descending Fisher-Yates chooses j in [0,i]. Hash `khminikey-window-v1\0`, the
32-byte big-endian seed, and the 32-byte big-endian counter using SHA-256. The
counter starts at zero and advances for every draw. Mask the first digest byte
to the smallest all-ones mask covering i and reject values above i. Counter
overflow or 1,024 rejected attempts fails closed. This specifies portable order;
the seed is not a security key or a throughput promise.

## Recovery and compatibility

Restart reconstructs the saved missing complement and resets the stream.
Seed, window, batch/work geometry, order and device may change. Sequence
reproducibility requires identical initial gaps, geometry, sizing and outcomes;
canonical ordinal coverage and deduplicated results define exact recovery.

No job/configuration/target identity, schema 7, protocol, capability or
`minikey-ordinal-v1` change. Existing minikey-capable workers may resume forward.
Start/summary and worker completion records expose `ordinal_seed` and
`ordinal_window`. Creation and non-minikey runners reject the new options.
Scalar `--order`, BSGS tile settings and coordinator block claims remain separate.

## Acceptance plan and remaining scope

Check an independent integer/hashlib model, fragmented gaps, seed endpoints,
window bounds, changing batch/work sizes, overflow retries, ownership and all
previous orders. Exercise both lengths/encodings, native device execution,
interrupted recovery, old-worker capabilities, pause/restore and supervised
HTTPS/disconnected transports on HIP and H200 CUDA. Keep source/binary identities,
executable examples and raw evidence under docs/. Scalar families' alternative
traversal remains pending; this slice does not complete all of C23.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
This published 30-character minikey matches at the highest ordinal of a
101-candidate range. Tiles of 17 candidates are shuffled in windows of four,
using seed `2a`. Run both blocks in the same Bash shell.

<!-- minikey-random-window-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
minikey_random_window_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-minikey-random-window.XXXXXX")"
export KEYHUNT_STATE_DIR="$minikey_random_window_dir/state"
printf '%s\n' 1CciesT23BNionJeXrbxmjc7ywfiyM4oLW > "$minikey_random_window_dir/addresses.txt"
"$KEYHUNT_BIN" minikeys inspect --key S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy > "$minikey_random_window_dir/inspect.json"
minikey_random_window_range="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o-100:x}:{o+1:x}")' "$minikey_random_window_dir/inspect.json")"
"$KEYHUNT_BIN" state project-create --name 'Public random-window minikey example' > "$minikey_random_window_dir/project.json"
minikey_random_window_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$minikey_random_window_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$minikey_random_window_project" --mode minikeys --length 30 \
  --range "$minikey_random_window_range" --block-width 65 --targets "$minikey_random_window_dir/addresses.txt" \
  > "$minikey_random_window_dir/job.json"
minikey_random_window_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$minikey_random_window_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$minikey_random_window_project" --job "$minikey_random_window_job" \
  --owner example --request first > "$minikey_random_window_dir/grant.json"
minikey_random_window_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$minikey_random_window_dir/grant.json")"
```

<!-- minikey-random-window-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" minikeys --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --range "$minikey_random_window_range" --targets "$minikey_random_window_dir/addresses.txt" \
  --batch-size 17 --ordinal-order random-window --ordinal-seed 2a --ordinal-window 4 > "$minikey_random_window_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_random_window_grant" --targets "$minikey_random_window_dir/addresses.txt" \
  --batch-size 17 --ordinal-order random-window --ordinal-seed 2a --ordinal-window 4 > "$minikey_random_window_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_random_window_grant" --targets "$minikey_random_window_dir/addresses.txt" \
  --ordinal-order reverse > "$minikey_random_window_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$minikey_random_window_project" --job "$minikey_random_window_job" \
  > "$minikey_random_window_dir/results.json"
"$KEYHUNT_BIN" state check > "$minikey_random_window_dir/check.json"
```

`results.json` contains the published minikey's uncompressed relation once.
The completed-grant reverse retry reports 101 resumed ordinals and zero batches.
