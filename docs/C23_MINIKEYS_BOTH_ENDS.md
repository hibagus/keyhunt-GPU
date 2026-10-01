# C23: exact both-ends minikey traversal

This slice adds execution-only `--ordinal-order both-ends` to native minikey
search, checkpoint runs and supervised workers. It covers the 22- and
30-character domains and all supported public-key encodings. Forward remains
the default; forward and reverse retain their existing sequences.

## Selection and recovery contract

Start at the lowest uncovered ordinal, then alternate low and high after each
successfully verified batch. A low batch maps logical lanes upward; a high batch
maps them downward. Selection uses the global endpoints of the missing interval
set, including fragmented recovered grants. GPU lanes still run concurrently.

Overflow advances neither coverage nor the alternating phase. Replay shrinks
from the same endpoint. Output and receipt validation precede advancement.
Receipts remain ascending half-open intervals of canonical minikey ordinals;
results retain their actual ordinal and target relation.

Adaptive work reservations are contiguous and cannot overlap or cross saved
coverage. Batch alternation continues inside large reservations. When the fronts
meet, they share the existing reservation and clip to its remaining span. Each
reservation records only its own active execution time, including replay; pauses
and time spent on the opposite front are excluded. At most two reservations are
active, independent of the size of the ordinal domain.

Restart reconstructs missing intervals from durable receipts and starts low.
It may switch among forward, reverse and both-ends, change batch geometry, or
move to a compatible worker. Alternating phase is not persisted: exact coverage
is the recovery contract. Prepared GPU targets survive direction and grant
changes, with direction taken from each submitted batch.

## Compatibility and scope

Job/configuration/target identities, schema 7, protocol, capability lists and
`minikey-ordinal-v1` coordinates remain unchanged. Existing `minikeys-v1` workers
can recover these jobs forward. Block claim policy remains independent of the
within-grant ordinal order. Scalar `--order` and BSGS `--tile-order` are separate.
`checkpoint create` and non-minikey runners reject `--ordinal-order`, even when
forward is explicit. Scalar strides, orbit expansion, stepped/GLV kernels and
other lengths remain unsupported for minikeys.

## Acceptance plan

Check independent candidate/sequence oracles for both lengths, tiny and huge
intervals, work reservations meeting at the middle, changing batch/work sizes,
base-58/limb carries and exact endpoints. Preserve forward/reverse regression
coverage, overflow replay, all encodings and all visible device ordinals.

Exercise fragmented receipt recovery, acknowledgment loss, killed restarts in
and out of both-ends, adaptive work, pause/restore/visibility changes, old-worker
compatibility and byte-identical upload retry. Check supervised HTTPS and
manual disconnected file transports on HIP and CUDA. Record executable examples,
source/binary identities and raw evidence under docs/.

## Remaining C23 scope

Additional random traversal semantics and other minikey orders remain pending.
This policy makes no throughput or fleet-scaling claim.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
This published 30-character example matches at the highest ordinal of a
101-candidate range. With batches of 17, the first batch starts low and the
second starts high and includes the public candidate. Six batches cover the
range exactly. Run both blocks in the same Bash shell.

<!-- minikey-both-ends-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
minikey_both_ends_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-minikey-both-ends.XXXXXX")"
export KEYHUNT_STATE_DIR="$minikey_both_ends_dir/state"
printf '%s\n' 1CciesT23BNionJeXrbxmjc7ywfiyM4oLW > "$minikey_both_ends_dir/addresses.txt"
"$KEYHUNT_BIN" minikeys inspect --key S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy > "$minikey_both_ends_dir/inspect.json"
minikey_both_ends_range="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o-100:x}:{o+1:x}")' "$minikey_both_ends_dir/inspect.json")"
"$KEYHUNT_BIN" state project-create --name 'Public both-ends minikey example' > "$minikey_both_ends_dir/project.json"
minikey_both_ends_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$minikey_both_ends_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$minikey_both_ends_project" --mode minikeys --length 30 \
  --range "$minikey_both_ends_range" --block-width 65 --targets "$minikey_both_ends_dir/addresses.txt" \
  > "$minikey_both_ends_dir/job.json"
minikey_both_ends_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$minikey_both_ends_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$minikey_both_ends_project" --job "$minikey_both_ends_job" \
  --owner example --request first > "$minikey_both_ends_dir/grant.json"
minikey_both_ends_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$minikey_both_ends_dir/grant.json")"
```

<!-- minikey-both-ends-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" minikeys --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --range "$minikey_both_ends_range" --targets "$minikey_both_ends_dir/addresses.txt" \
  --batch-size 17 --ordinal-order both-ends > "$minikey_both_ends_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_both_ends_grant" --targets "$minikey_both_ends_dir/addresses.txt" \
  --batch-size 17 --ordinal-order both-ends > "$minikey_both_ends_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_both_ends_grant" --targets "$minikey_both_ends_dir/addresses.txt" \
  --ordinal-order reverse > "$minikey_both_ends_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$minikey_both_ends_project" --job "$minikey_both_ends_job" \
  > "$minikey_both_ends_dir/results.json"
"$KEYHUNT_BIN" state check > "$minikey_both_ends_dir/check.json"
```

`results.json` contains the published minikey's uncompressed relation once.
The completed-grant reverse retry reports 101 resumed ordinals and zero batches.
