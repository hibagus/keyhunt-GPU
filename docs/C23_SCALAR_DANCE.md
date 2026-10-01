# C23: dance scalar batches

Status: implemented. Acceptance requirements are listed below.

`--batch-order dance` applies to xpoint, Bitcoin address/HASH160, Ethereum and
Bitcoin vanity, through native searches, checkpoint run, worker run-device and
the Python supervisor. Default remains forward. Checkpoint creation, BSGS and
minikeys reject this scalar-only option, including explicit forward.

Dance repeats low, high, middle after each accepted batch. At invocation start,
let L be the lowest missing coordinate and H the highest missing exclusive end.
The fixed pivot is P = L + floor((H - L) / 2). Subtraction precedes addition to
avoid overflow. Split any missing interval crossing P. Low selects the global
lowest endpoint; high selects the global highest endpoint; middle selects the
lowest remaining endpoint at or above P, falling back to global low when none
remains. The pivot does not move during an invocation, including across saved
holes. Empty coverage requires no pivot or live executor generation.

Selection uses the job's existing receipt coordinates: actual scalars for an
unmapped search and canonical candidate indices for strides, reverse mappings
and six-member orbits. Within every batch these coordinates increase; immutable
`--order forward|reverse` still controls their scalar mapping. Orbit batches stay
within one variant, including a high batch's lower boundary. There is no change
to identities, configuration, schema, receipts, protocol or capabilities.

Each endpoint reserves a contiguous adaptive work unit clipped to its missing
interval, the pivot and UINT64_MAX. At most three reservations are live. Fronts
that meet share the original owner; each owner starts and finishes once. Only
its own execution and replay time contributes to adaptive sizing. Pauses and
execution of other owners are excluded. A map lookup finds the middle endpoint
without scanning the remaining intervals; the pivot adds at most one interval.

Overflow credits nothing and retries the same phase and endpoint with a smaller
batch limit. Planning does not advance coverage or phase; acceptance follows
successful output or durable receipt handling. Batches never cross saved
coverage, work boundaries or the pivot. Restart uses the exact saved complement,
computes its new fixed pivot, and starts low. Batch order, geometry and device
may change; the saved scalar mapping must still match. Coordinator claim order
is independent of selection within a grant. Telemetry reports `batch_order`.

For unmapped [1,102), work and batch widths 17 give P=51 and this sequence:
[1,18), [85,102), [51,68), [18,35), [68,85), [35,51).
The last middle phase falls back to low. This deterministic traversal makes no
throughput or discovery-probability claim.

Acceptance requires independent sequence and ownership checks, including retries,
fragmented gaps, pivot boundaries, changing geometry, wide coordinates, all scalar
mappings and orbit clipping; native HIP/CUDA validation for all scalar families
and kernels; durable recovery and completed retries; online and offline worker
parity; and a verbatim executable public example. Scalar random-window traversal
remains a separate pending C23 slice.

## Executable public example

This uses the published compressed Bitcoin address for scalar 1. Choose
`KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Both blocks run in the same Bash shell. The hexadecimal range `1:66` contains
101 scalars; decimal batches of 17 visit low, high and middle in turn.
The fixed midpoint is 51; the final middle selection falls back to low.

<!-- scalar-dance-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
scalar_example_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-scalar-dance.XXXXXX")"
export KEYHUNT_STATE_DIR="$scalar_example_dir/state"
printf '%s\n' 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH > "$scalar_example_dir/addresses.txt"
"$KEYHUNT_BIN" state project-create --name 'Public dance scalar example' > "$scalar_example_dir/project.json"
scalar_example_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$scalar_example_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$scalar_example_project" --mode address \
  --range 1:66 --block-width 65 --targets "$scalar_example_dir/addresses.txt" > "$scalar_example_dir/job.json"
scalar_example_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$scalar_example_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$scalar_example_project" --job "$scalar_example_job" \
  --owner example --request first > "$scalar_example_dir/grant.json"
scalar_example_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$scalar_example_dir/grant.json")"
```

<!-- scalar-dance-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" address --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:66 --targets "$scalar_example_dir/addresses.txt" --batch-size 17 \
  --batch-order dance > "$scalar_example_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-size 17 --batch-order dance > "$scalar_example_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-order forward > "$scalar_example_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$scalar_example_project" --job "$scalar_example_job" \
  > "$scalar_example_dir/results.json"
"$KEYHUNT_BIN" state check > "$scalar_example_dir/check.json"
```

The result is scalar 1 once. The completed forward retry submits zero batches.
