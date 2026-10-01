# C23: related-key orbit expansion

The selected slice adds explicit
`--endomorphism orbit` to native xpoint, Bitcoin HASH160/P2PKH, Ethereum and
vanity. Default `none` preserves existing behavior. This is distinct from
`--kernel glv`, which changes only the multiplication implementation.

## Coverage contract

The input range and positive stride describe **seeds**, not a bound on every
searched private scalar. For each nonzero seed k, search these six variants in
order: `k`, `-k`, `lambda*k`, `-lambda*k`, `lambda²*k`, `-lambda²*k`, modulo n.
The endomorphism has six distinct scalar members for every nonzero seed.
Xpoint intentionally retains both signs even though their X coordinates agree.

For N seeds, expanded indices are `[1,6*N+1)`. Enumeration is variant-major:
index `j = v*N+i`, where `v=0..5`, `i=1..N`. Forward/reverse selects seed order
within each variant; reverse does not reverse the variant order. Results expose
`candidate_index`, `seed_scalar`, `orbit_variant` and the actual `scalar`, with
`coordinate_space:"scalar-orbit-index-v1"`. Coverage, block width, receipt fields
and computed/resumed counts use expanded candidate indices. Overlapping seed
orbits remain distinct candidate/target observations; no global deduplication is
claimed. Exhaustion certifies these candidates, not a contiguous scalar range.

The existing 256-bit journal coordinate domain requires `6*N+1 <= n`. Oversized
jobs fail before creation; split the seed range into smaller jobs. No candidate
count may truncate or wrap. Empty/invalid seeds, nonpositive strides and unknown
variants are rejected.

## Identity, execution and compatibility

Configuration version 4 means forward orbit expansion and version 5 means
reverse-seed orbit expansion. Both retain the 146-byte indexed layout containing
original A, B and S; schema 7 and receipt bytes stay unchanged. The expanded root
must match exactly. Existing versions 1/2/3 and explicit `none` preserve their
identities. Orbit and ordinary receipts are never interchangeable.

Kernel batches stop at variant boundaries, including when a block starts or
ends inside a variant. Direct/GLV kernels derive the seed point and apply the
selected endomorphism/sign. Stepped execution uses the transformed seed point
and transformed positive/negative point-step cache. CPU verification derives the
actual private scalar independently before accepting a relation. Every completed
candidate counts once; whole-attempt overflow/replay rules remain unchanged.

Checkpoint runs infer the saved mapping and reject explicit conflicts. Workers
must advertise `scalar-orbit-v1` before allocation, renewals, updates or cached
responses for version-4/5 jobs. Old capability sets retain their supported jobs.
Fresh device owners self-test orbit mapping; prepared allocations survive grant
handoff. HTTPS and disconnected file transport share the exact binding.
BSGS and minikey enumeration do not accept this option in this slice.

## Acceptance gates

Independent integer and pinned full-public-key oracles cover all six members,
near-order/high-bit seeds, bounds, variant transitions and overlapping orbits.
HIP and CUDA must agree for all four families, all three kernels, both seed
orders, unit/wide strides, short tails, overflow and all visible devices.
Durable gates cover malformed bindings, incompatible worker fences, repeated
results at different indices, killed restarts, pause/visibility changes and
exact local/server reconciliation over both transports. Document findings and
source/binary-bound evidence under docs/; commit each logical change separately.

## Host mapping finding

The first host gate passed 7048 independent integer cases, exhaustive small
block/work/batch partitions, candidate-domain bounds, and existing forward/
reverse/work-unit regressions. Batches stop at each variant boundary even when
a work unit spans more than one variant; the next call continues its exact tail.

## GPU mapping finding

The first HIP search pass completed all 17 gates: 164 CLI cases across the four
families, three kernels and both seed orders; independent public-key orbit
oracles; host mapping gates; and existing executor/failure regressions. Wide
strides, near-order seeds, duplicate seed orbits, variant boundaries, overflow
replay and every visible MI300X passed. CUDA and durable acceptance follow.

## Executable public example

Choose `KEYHUNT_BIN` and `GPU_BACKEND` as in [GPU_QUICKSTART.md](GPU_QUICKSTART.md).
The following blocks run in the same Bash shell. Three seeds (`1`, `2`, `3`)
produce 18 candidate observations. The generator's X coordinate matches scalar
1 and scalar n−1, although n−1 lies outside the seed range. Reverse seed order
places these at candidate indices 3 and 6, with variants 0 and 1.

<!-- orbit-example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable}"
umask 077
orbit_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-orbit.XXXXXX")"
export KEYHUNT_STATE_DIR="$orbit_dir/state"
printf '%s\n' 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$orbit_dir/xpoint.txt"
"$KEYHUNT_BIN" state project-create --name 'Public orbit example' > "$orbit_dir/project.json"
orbit_project="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$orbit_dir/project.json")"
"$KEYHUNT_BIN" checkpoint create --project "$orbit_project" --mode xpoint \
  --range 1:4 --endomorphism orbit --order reverse --block-width 12 \
  --targets "$orbit_dir/xpoint.txt" > "$orbit_dir/job.json"
orbit_job="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$orbit_dir/job.json")"
"$KEYHUNT_BIN" state claim --project "$orbit_project" --job "$orbit_job" \
  --owner example --request first > "$orbit_dir/grant.json"
orbit_grant="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$orbit_dir/grant.json")"
```

Range and block-width values are hexadecimal: `12` means 18 expanded candidates.
The saved mapping is inferred during checkpoint execution. Kernel choice can
change on restart; an explicit conflicting endomorphism or order is rejected.

<!-- orbit-example: execute -->
```bash
: "${GPU_BACKEND:?choose hip or cuda}"
"$KEYHUNT_BIN" xpoint --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --range 1:4 --endomorphism orbit --order reverse --kernel glv \
  --targets "$orbit_dir/xpoint.txt" > "$orbit_dir/volatile.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$orbit_grant" --targets "$orbit_dir/xpoint.txt" --kernel stepped \
  > "$orbit_dir/durable.ndjson"
"$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "${GPU_DEVICE:-0}" \
  --grant "$orbit_grant" --targets "$orbit_dir/xpoint.txt" --kernel direct \
  > "$orbit_dir/retry.ndjson"
"$KEYHUNT_BIN" checkpoint results --project "$orbit_project" --job "$orbit_job" \
  > "$orbit_dir/results.json"
"$KEYHUNT_BIN" state check > "$orbit_dir/check.json"
```

Inspect `results.json`: each row distinguishes its expanded index, original seed,
variant and actual private scalar. `retry.ndjson` reports 18 resumed candidates
and zero new batches. The same options apply to `hash160`, `address`, `ethereum`
and `vanity`; their target formats are unchanged.

## Durable findings

The initial CPU suite passed 106 of 107 tests. The remaining older stride test
removed two entries from the new nine-capability list, accidentally leaving a
valid seven-capability worker. Its denial fixture now truncates to six; the
focused rerun passed. No production capability rule changed for this correction.
Five focused ASan/UBSan gates passed, including host mapping, independent integer
and portable point oracles, four-family journal recovery and coordinator fences.
Full hardware observations and limits are recorded in [acceptance](C23_ORBITS_VALIDATION.md).

Orbit cache memory is six times the existing stepped point-power cache, not six
times the targets or output buffer. Each variant is injective over the seed set;
stopping batches at variant boundaries preserves xpoint's compact per-batch
`2*targets` output bound even when an entire job contains repeated orbits.
CLI `scalar_begin`/`scalar_end_exclusive` metadata denotes the original seed
range for orbit jobs. Expanded `begin`/`end_exclusive` denotes candidate coverage.
This slice establishes correctness and recovery, without a throughput claim.
