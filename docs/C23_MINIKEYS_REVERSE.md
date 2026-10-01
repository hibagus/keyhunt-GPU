# C23: exact reverse minikey traversal

This slice adds execution-only `--ordinal-order forward|reverse` to native
minikey search, checkpoint runs and supervised workers. Both 22- and
30-character formats and all existing encoding selections are supported.
Forward remains the default. This option is separate from immutable scalar
`--order` and from BSGS `--tile-order`.

## Mapping and coverage

Direction changes scheduling, not the canonical ordinal-to-text mapping. Reverse
selects the highest missing interval, reserves work from its upper endpoint and
submits bounded batches downward. For a batch [L,H), logical lane i represents
ordinal H-1-i. GPU lanes execute concurrently; direction describes this mapping
and the submission sequence, not a serial wall-clock execution guarantee.

Every ordinal is tested, including candidates rejected by their check byte. CPU
verification derives the ordinal from the batch and lane, reconstructs the text,
and validates the private scalar and encoding/HASH160 relation. Match and receipt
coordinates remain actual one-based minikey ordinals (`minikey-ordinal-v1`).

An internal reverse-minikey execution tag distinguishes lane interpretation and
is included in completion identity checks. It is not serialized as a new job
configuration. A scalar mapping cannot be attached to either minikey algorithm.
The GPU executor reads direction from each submitted batch, allowing preparation
to survive grant boundaries without retaining stale direction state.

Overflow invalidates the whole attempt. Replay shrinks from the same high
endpoint, then advances only after verified completion. Adaptive work units are
contiguous and cannot cross saved coverage. Pause drains the admitted batch;
restart reconstructs the exact complement and may change direction or geometry.

## Compatibility and scope

Job/configuration/target identity, schema 7, coordinator protocol, capability
lists and result coordinates remain unchanged. Existing `minikeys-v1` workers
can execute the same jobs forward, including after recovery from a reverse
owner. Block claim policy still selects grants independently of ordinal order.
`checkpoint create` and non-minikey runners reject `--ordinal-order`, including
explicit forward. Minikey scalar strides, scalar `--order`, orbit expansion,
stepped/GLV kernels, other lengths and random traversal remain unsupported.

## Acceptance plan

Use independent integer sequence and candidate/hash/public-key oracles across
both lengths, base-58 and limb boundaries, domain endpoints, changing batches,
all visible devices, forced overflow and both encodings. Check batch identities,
invalid options and wrong-mode rejection before coverage can be written.

Exercise acknowledgment loss, killed restarts in both directions, fixed/adaptive
work, pause/restore/visibility changes, fragmented grant recovery, old-worker
capability compatibility, byte-identical upload retries, HTTPS and disconnected
file workers. Keep executable examples and raw evidence under docs/.

## Remaining C23 scope

Additional random traversal semantics and other minikey orders remain pending.
This slice makes no throughput or fleet-scaling claim.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
This published 30-character example matches at the highest ordinal in a
101-candidate range. The first reverse batch includes that candidate. Run both
blocks in the same Bash shell; all paths stay in a temporary directory.

<!-- minikey-reverse-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
minikey_reverse_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-minikey-reverse.XXXXXX")"
export KEYHUNT_STATE_DIR="$minikey_reverse_dir/state"
printf '%s\n' 1CciesT23BNionJeXrbxmjc7ywfiyM4oLW > "$minikey_reverse_dir/addresses.txt"
"$KEYHUNT_BIN" minikeys inspect --key S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy > "$minikey_reverse_dir/inspect.json"
minikey_reverse_range="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o-100:x}:{o+1:x}")' "$minikey_reverse_dir/inspect.json")"
"$KEYHUNT_BIN" state project-create --name 'Public reverse minikey example' > "$minikey_reverse_dir/project.json"
minikey_reverse_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$minikey_reverse_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$minikey_reverse_project" --mode minikeys --length 30 \
  --range "$minikey_reverse_range" --block-width 65 --targets "$minikey_reverse_dir/addresses.txt" \
  > "$minikey_reverse_dir/job.json"
minikey_reverse_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$minikey_reverse_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$minikey_reverse_project" --job "$minikey_reverse_job" \
  --owner example --request first > "$minikey_reverse_dir/grant.json"
minikey_reverse_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$minikey_reverse_dir/grant.json")"
```

<!-- minikey-reverse-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" minikeys --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --range "$minikey_reverse_range" --targets "$minikey_reverse_dir/addresses.txt" \
  --batch-size 17 --ordinal-order reverse > "$minikey_reverse_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_reverse_grant" --targets "$minikey_reverse_dir/addresses.txt" \
  --batch-size 17 --ordinal-order reverse > "$minikey_reverse_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" --length 30 \
  --grant "$minikey_reverse_grant" --targets "$minikey_reverse_dir/addresses.txt" \
  --ordinal-order forward > "$minikey_reverse_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$minikey_reverse_project" --job "$minikey_reverse_job" \
  > "$minikey_reverse_dir/results.json"
"$KEYHUNT_BIN" state check > "$minikey_reverse_dir/check.json"
```

`results.json` contains the published minikey's uncompressed relation once.
The forward retry reports 101 resumed ordinals and zero batches for the same job.
