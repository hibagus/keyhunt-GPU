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

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
This published 30-character example matches at the highest ordinal of a
101-candidate range. With batches of 17, the first batch starts low and the
second starts high and includes the public candidate. The fixed pivot splits this range into 50 and 51 ordinals; six batches cover
the range exactly, including the middle front in the third batch. Run both blocks in the same Bash shell.

<!-- minikey-dance-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
minikey_dance_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-minikey-dance.XXXXXX")"
export KEYHUNT_STATE_DIR="$minikey_dance_dir/state"
printf '%s\n' 1CciesT23BNionJeXrbxmjc7ywfiyM4oLW > "$minikey_dance_dir/addresses.txt"
"$KEYHUNT_BIN" minikeys inspect --key S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy > "$minikey_dance_dir/inspect.json"
minikey_dance_range="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o-100:x}:{o+1:x}")' "$minikey_dance_dir/inspect.json")"
"$KEYHUNT_BIN" state project-create --name 'Public dance minikey example' > "$minikey_dance_dir/project.json"
minikey_dance_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$minikey_dance_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$minikey_dance_project" --mode minikeys --length 30 \
  --range "$minikey_dance_range" --block-width 65 --targets "$minikey_dance_dir/addresses.txt" \
  > "$minikey_dance_dir/job.json"
minikey_dance_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$minikey_dance_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$minikey_dance_project" --job "$minikey_dance_job" \
  --owner example --request first > "$minikey_dance_dir/grant.json"
minikey_dance_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$minikey_dance_dir/grant.json")"
```

<!-- minikey-dance-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" minikeys --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --range "$minikey_dance_range" --targets "$minikey_dance_dir/addresses.txt" \
  --batch-size 17 --ordinal-order dance > "$minikey_dance_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_dance_grant" --targets "$minikey_dance_dir/addresses.txt" \
  --batch-size 17 --ordinal-order dance > "$minikey_dance_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_dance_grant" --targets "$minikey_dance_dir/addresses.txt" \
  --ordinal-order reverse > "$minikey_dance_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$minikey_dance_project" --job "$minikey_dance_job" \
  > "$minikey_dance_dir/results.json"
"$KEYHUNT_BIN" state check > "$minikey_dance_dir/check.json"
```

`results.json` contains the published minikey's uncompressed relation once.
The completed-grant reverse retry reports 101 resumed ordinals and zero batches.
