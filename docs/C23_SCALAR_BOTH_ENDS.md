# C23: both-ends scalar batches

Status: implemented. Hardware acceptance and limits are recorded in
[C23_SCALAR_BOTH_ENDS_VALIDATION.md](C23_SCALAR_BOTH_ENDS_VALIDATION.md).

`--batch-order forward|both-ends` selects execution order for xpoint, Bitcoin
address/HASH160, Ethereum and Bitcoin vanity. It is available on native searches,
checkpoint run, worker run-device and the Python supervisor. Default is forward.
Checkpoint creation, BSGS and minikeys reject it, including explicit forward.

This execution setting does not change `--order forward|reverse`, which is an
immutable scalar mapping. Work is selected in the job's existing receipt
coordinates: actual scalars for unmapped searches, candidate indices for strides,
reverse mappings and six-member orbits. No identity, configuration, schema,
receipt, protocol or capability version changes. Older compatible workers can
continue the same job in forward batch order.

Both-ends begins low and alternates low/high after each accepted batch. Each side
reserves a contiguous adaptive work unit clipped to its missing interval and
UINT64_MAX. At most two reservations are live; when fronts meet they share the
original reservation. A batch never crosses saved coverage or a work boundary.
Within each batch canonical coordinates increase, preserving the job's existing
scalar mapping, kernel and verification rules. A high batch in an orbit stops at
its last variant's lower boundary, so it never mixes variants. Batch order is not
an individual descending lane mapping.

Overflow credits nothing and does not alternate: the same endpoint is retried
with a smaller bound. Accepted low batches remove a prefix; accepted high batches
remove a suffix. Growth is clipped to the remaining work. Each reservation starts
once and finishes once, charging only its own execution and replay time. Pauses
and execution of other reservations are excluded from adaptive timing. Planning
and accepting are separate; output or receipt failure cannot advance coverage.

Restart takes the exact saved complement, starts low again, and may change batch
order, geometry, device or work sizing. The immutable scalar mapping still must
match. Coordinator claim order remains independent of order within a grant.
Native start/summary, checkpoint summary and worker grant-finish report
`batch_order`. No throughput or discovery-probability improvement is claimed.

For an unmapped `[1,18)` range, work width 5 and batch width 3, accepted intervals
are `[1,4)`, `[15,18)`, `[4,6)`, `[13,15)`, `[6,9)`, `[11,13)`, `[9,11)`.
With work width 5 and batch width 2 over `[1,6)`, the fronts share one owner:
`[1,3)`, `[4,6)`, `[3,4)`. With reverse scalar mapping,
these same canonical intervals map through the saved descending progression.

Acceptance requires independent integer sequence/coverage checks, orbit boundary
checks, invalid option rejection, all scalar families and kernels on HIP/CUDA,
exact overflow and restart recovery, and online/offline worker result parity.
Findings and evidence will be recorded in a separate validation document.

## Executable public example

This uses the published compressed Bitcoin address for scalar 1. Choose
`KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
Both blocks run in the same Bash shell. The hexadecimal range `1:66` contains
101 scalars; decimal batches of 17 visit low, high, low, high, low, high.

<!-- scalar-both-ends-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
scalar_example_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-scalar-both-ends.XXXXXX")"
export KEYHUNT_STATE_DIR="$scalar_example_dir/state"
printf '%s\n' 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH > "$scalar_example_dir/addresses.txt"
"$KEYHUNT_BIN" state project-create --name 'Public both-ends scalar example' > "$scalar_example_dir/project.json"
scalar_example_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$scalar_example_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$scalar_example_project" --mode address \
  --range 1:66 --block-width 65 --targets "$scalar_example_dir/addresses.txt" > "$scalar_example_dir/job.json"
scalar_example_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$scalar_example_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$scalar_example_project" --job "$scalar_example_job" \
  --owner example --request first > "$scalar_example_dir/grant.json"
scalar_example_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$scalar_example_dir/grant.json")"
```

<!-- scalar-both-ends-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" address --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:66 --targets "$scalar_example_dir/addresses.txt" --batch-size 17 \
  --batch-order both-ends > "$scalar_example_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-size 17 --batch-order both-ends > "$scalar_example_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$scalar_example_grant" --targets "$scalar_example_dir/addresses.txt" --input-format address \
  --batch-order forward > "$scalar_example_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$scalar_example_project" --job "$scalar_example_job" \
  > "$scalar_example_dir/results.json"
"$KEYHUNT_BIN" state check > "$scalar_example_dir/check.json"
```

The result is scalar 1 once. The completed forward retry submits zero batches.
