# C23: seeded scalar random windows

`--batch-order random-window` selects execution order for xpoint, Bitcoin
address/HASH160, Ethereum and vanity. Native searches, checkpoint runs and workers
accept `--batch-seed HEX` (256 bits, default zero) and `--batch-window 1..256`
(default 64). Explicit seed/window options require random-window. Forward remains
the default. Creation, BSGS and minikey searches reject these scalar options.

## Window and ownership contract

At window creation, sample the current batch limit B and work span. Form up to W
ascending tiles of at most B canonical coordinates from the lowest missing gaps.
Tiles and owners cannot cross saved coverage. Partition owners before shuffling:
owner tile count is floor(min(work span, UINT64_MAX) / B), with minimum one and
maximum W. Gap and window tails clip owners. Shuffle these tiles and exhaust the
window before forming another. State beyond input gaps is bounded by W tiles and
W owners; domains are never expanded into individual coordinates.

A selected tile can span orbit variants. Each submitted batch still ends at the
current variant boundary, its tile boundary or the current batch limit, whichever
comes first. Coordinates increase inside every batch and tile. The saved scalar
mapping (`--order`, stride and optional orbit) remains unchanged. This applies
both to unmapped scalar coordinates and mapped candidate indices.

Overflow credits nothing, consumes no random draw and retries the same tile
suffix with a smaller limit. Successful sub-batches consume only their accepted
interval. The tile stays selected until exhausted; growth cannot cross its end.
An owner is announced once on its first submission, including an overflow, and
finishes on its final accepted sub-batch in shuffled order. Adaptive timing charges
only that owner's execution and replay time, excluding pauses and other owners.
New geometry applies to the next window. At most W extra accepted fragments may
exist while completing a window.

## Portable shuffle

Use descending Fisher-Yates. For each draw, SHA-256 hashes the bytes
`khscalar-window-v1\0`, the 32-byte big-endian seed and the 32-byte big-endian
counter. The counter starts at zero and advances on every draw. Mask the first
hash byte with the smallest all-ones mask covering the current largest index;
reject values above that index. Counter overflow or 1,024 rejected draws fails
closed. Window size one draws nothing and is sequential. Seed zero is valid.

This shuffles locally within ascending windows. It does not provide uniform
selection over the entire domain or sampling with replacement. The seed is an
ordering parameter, not a security key or a performance promise.

## Restart and compatibility

Restart reconstructs the exact missing complement and resets the shuffle stream.
Order, seed, window size, geometry and device can change. Reproducing a sequence
requires identical starting gaps, geometry, adaptive observations and outcomes;
exact recovery is defined by canonical coverage and deduplicated results.

No job/configuration identity, schema 7, receipt, protocol or capability changes.
Older compatible workers can resume in forward batch order. Coordinator block
claim order is separate. Native start/summary, checkpoint summary and worker
completion records expose `batch_order`, and random-window records also expose
`batch_seed` and `batch_window`.

## Acceptance requirements

Check independent integer/hashlib sequences, mapping and ownership, fragmented
windows, seed endpoints, rejection sampling, window bounds, changing batch/work
limits, retries and orbit clipping. Exercise all scalar families and kernels on
HIP and H200, killed-process recovery, all policy transitions, changed seeds and
windows, completed retries, pause/restore and HTTPS/disconnected workers. Bind
executable examples and raw evidence to source and binary hashes under docs/.
C23 completion depends on these gates passing; acceptance is recorded separately.

## Executable public example

This uses the published compressed Bitcoin address for scalar 1. Choose
`KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Both blocks run in the same Bash shell. The hexadecimal range `1:66` contains
101 scalars. Windows of four tiles, batch width 17 and seed `2a` visit
[1,18), [35,52), [18,35), [52,69), [86,102), [69,86).

<!-- scalar-random-window-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
scalar_example_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-scalar-random-window.XXXXXX")"
export KEYHUNT_STATE_DIR="$scalar_example_dir/state"
printf '%s\n' 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH > "$scalar_example_dir/addresses.txt"
"$KEYHUNT_BIN" state project-create --name 'Public random-window scalar example' > "$scalar_example_dir/project.json"
scalar_example_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$scalar_example_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$scalar_example_project" --mode address \
  --range 1:66 --block-width 65 --targets "$scalar_example_dir/addresses.txt" > "$scalar_example_dir/job.json"
scalar_example_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$scalar_example_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$scalar_example_project" --job "$scalar_example_job" \
  --owner example --request first > "$scalar_example_dir/grant.json"
scalar_example_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$scalar_example_dir/grant.json")"
```

<!-- scalar-random-window-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" address --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:66 --targets "$scalar_example_dir/addresses.txt" --batch-size 17 \
  --batch-order random-window --batch-seed 2a --batch-window 4 > "$scalar_example_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-size 17 --batch-order random-window --batch-seed 2a --batch-window 4 > "$scalar_example_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-order forward > "$scalar_example_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$scalar_example_project" --job "$scalar_example_job" \
  > "$scalar_example_dir/results.json"
"$KEYHUNT_BIN" state check > "$scalar_example_dir/check.json"
```

The result is scalar 1 once. The completed forward retry submits zero batches.
