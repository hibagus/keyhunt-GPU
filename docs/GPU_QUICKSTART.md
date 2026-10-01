# Finite HIP and CUDA searches

This example uses the public secp256k1 generator (scalar 1) and two published minikeys. It exercises
bounded xpoint/BSGS/Bitcoin HASH160/Ethereum/vanity/minikey searches and positive scalar strides, real-input job creation, sequential/random claims,
local checkpoints and completed-grant replay. Follow [BUILD.md](BUILD.md) first.
No coordinator is required. Run the blocks below in order in the **same Bash
shell**, starting in the repository root. Use a new private example directory
for each run; the commands do not modify an existing job.

## Choose a backend

For MI300X:

```sh
export KEYHUNT_BIN="$PWD/build/hip-release/keyhunt"
export GPU_BACKEND=hip
export GPU_DEVICE=0
```

For H200, use these settings instead:

```sh
export KEYHUNT_BIN="$PWD/build/cuda-h200/keyhunt"
export GPU_BACKEND=cuda
export GPU_DEVICE=0
```

`GPU_DEVICE` is the ordinal visible to this process. For example, after setting
`CUDA_VISIBLE_DEVICES=7`, ordinal 0 refers to the previously visible device 7.
HIP uses runtime visibility settings such as `HIP_VISIBLE_DEVICES`; retain any
existing administrator-provided mask. Record UUIDs from `devices` before changing
visibility. [Supervised queues](MULTI_GPU.md#fleet-supervision) retain their UUID
binding and need an explicit queue-to-ordinal mapping on a changed mask.

## Prepare the public fixture

The directory is under `/var/tmp` because journals reject paths inside **any**
Git checkout, including an enclosing `/tmp/.git`. Set `EXAMPLE_PARENT` to another private local
parent if `/var/tmp` is unavailable. `umask` makes the files private. Keep this
shell and directory if you want to inspect or resume the example later.

<!-- example: prepare -->
```bash
set -euo pipefail
: "${KEYHUNT_BIN:?choose the built executable first}"
: "${GPU_BACKEND:?choose hip or cuda first}"
GPU_DEVICE="${GPU_DEVICE:-0}"
umask 077
example_dir="$(mktemp -d "${EXAMPLE_PARENT:-/var/tmp}/keyhunt-example.XXXXXX")"
export KEYHUNT_STATE_DIR="$example_dir/state"
printf '%s\n' "$example_dir"
printf '%s\n' 79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$example_dir/xpoint.txt"
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 \
  > "$example_dir/public-key.txt"
printf '%s\n' 751e76e8199196d454941c45d1b3a323f1433bd6 \
  91b24bf9f5288532960ac687abb035127b1d28a5 > "$example_dir/hash160.txt"
printf '%s\n' 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH \
  1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm > "$example_dir/address.txt"
printf '%s\n' 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf \
  > "$example_dir/ethereum.txt"
printf '%s\n' 1BgGZ9tc 1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH \
  1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm > "$example_dir/vanity.txt"
printf '%s\n' 5ac36b4aff945da30c16bf6c25dbac434664da8c \
  7ac00f979ff0df2fdcb65761dc8f9ef8b37142db > "$example_dir/minikeys22.txt"
"$KEYHUNT_BIN" minikeys inspect --key SzavMBLoXU6kDrqtUVmffv \
  > "$example_dir/minikeys22-inspect.json"
printf '%s\n' f78c1591f3f34fd1fe339dc371069b7b492bf370 \
  7f6ab65fa911f558ca2dde3e9d073acb02c0d5c6 > "$example_dir/minikeys30.txt"
"$KEYHUNT_BIN" minikeys inspect --key S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy \
  > "$example_dir/minikeys30-inspect.json"
"$KEYHUNT_BIN" bsgs-table build --m 257 --output "$example_dir/babies.khb" \
  > "$example_dir/table.json"
"$KEYHUNT_BIN" bsgs-table inspect --input "$example_dir/babies.khb" \
  > "$example_dir/table-inspect.json"
```

The versioned table is built on the CPU for either backend. Legacy `-S` caches
are incompatible; rebuild using `bsgs-table build`. The tiny `m=257` is a fixture,
not a general table-size recommendation ([table budgets](BSGS_TABLES.md)).

## Run finite searches

<!-- example: volatile -->
```bash
"$KEYHUNT_BIN" devices --backend "$GPU_BACKEND" > "$example_dir/devices.json"
"$KEYHUNT_BIN" gpu-smoke --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --steps 257 > "$example_dir/smoke.json"
"$KEYHUNT_BIN" bsgs-table validate --backend "$GPU_BACKEND" \
  --device "$GPU_DEVICE" --input "$example_dir/babies.khb" \
  > "$example_dir/table-validate.json"
"$KEYHUNT_BIN" xpoint --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:101 --targets "$example_dir/xpoint.txt" --batch-size 256 \
  > "$example_dir/xpoint.ndjson"
"$KEYHUNT_BIN" bsgs --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:10001 --targets "$example_dir/public-key.txt" \
  --table "$example_dir/babies.khb" --giant-batch 256 --target-batch 1 \
  > "$example_dir/bsgs.ndjson"
"$KEYHUNT_BIN" hash160 --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:101 --targets "$example_dir/hash160.txt" --encoding both --batch-size 256 \
  > "$example_dir/hash160.ndjson"
"$KEYHUNT_BIN" address --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:101 --targets "$example_dir/address.txt" --encoding both --batch-size 256 \
  > "$example_dir/address.ndjson"
"$KEYHUNT_BIN" ethereum --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:101 --targets "$example_dir/ethereum.txt" --batch-size 256 \
  > "$example_dir/ethereum.ndjson"
"$KEYHUNT_BIN" vanity --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
  --range 1:101 --targets "$example_dir/vanity.txt" --encoding both --batch-size 256 \
  > "$example_dir/vanity.ndjson"
# Hex stride 3 visits 1, 4, 7, ... below 0x301: exactly 256 candidates.
for mode in xpoint hash160 address ethereum vanity; do
  "$KEYHUNT_BIN" "$mode" --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
    --range 1:301 --stride 3 --targets "$example_dir/$mode.txt" --batch-size 256 \
    > "$example_dir/stride-$mode.ndjson"
done
for length in 22 30; do
  bounds="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o:x}:{o+256:x}")' "$example_dir/minikeys$length-inspect.json")"
  "$KEYHUNT_BIN" minikeys --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
    --length "$length" --input-format hash160 --range "$bounds" \
    --targets "$example_dir/minikeys$length.txt" --batch-size 256 \
    > "$example_dir/minikeys$length.ndjson"
done
```

All searches exit 0 and exhaust their exact ranges. Xpoint and BSGS find scalar 1
once. Ethereum finds its single Keccak address. HASH160 and address each find its compressed and uncompressed relations,
for two matches at the same scalar. Vanity finds three prefix/encoding relations at scalar 1, including the overlapping compressed prefixes. Range endpoints and block widths are **hexadecimal and half-open**: `1:101` covers
256 scalars and `1:10001` covers 65,536. Batch sizes and `m` are decimal. Xpoint
matches the full X coordinate and cannot distinguish the two Y signs; BSGS
matches the full public key. All modes verify returned candidates on the CPU.

`address` accepts Bitcoin mainnet P2PKH Base58Check only. `hash160` accepts
40 hexadecimal digits per line. `--encoding compressed|uncompressed|both` defaults
to `both`; it is part of immutable target identity. See [Bitcoin contracts](C23_HASH160.md).
`ethereum` accepts 40 hex digits with optional `0x`; mixed case must pass ERC-55.
It uses Keccak of the full affine public point and accepts no `--encoding` flag.
See [Ethereum contracts](C23_ETHEREUM.md).
`vanity` accepts case-sensitive Bitcoin P2PKH prefixes, one per line, beginning
with `1`. It checks the full Base58Check address, including checksum characters,
and retains every overlapping prefix relation. Encoding defaults to `both`.
Candidate capacity must fit the sum of distinct prefix lengths per encoding
(at most 68). See [vanity contracts](C23_VANITY.md).
`minikeys` requires length 22 or 30. Its range covers candidate ordinals: the
helper maps each public example to its exact ordinal. Both encoding relations
match at the starting ordinal; checksum-rejected candidates still count toward
coverage. Results show `ordinal`, `minikey` and the derived private `scalar`.
See [minikey contracts](C23_MINIKEYS.md).

The `stride-*` examples visit exactly `1 + i*3 < 0x301`, including scalar 1.
Their coverage intervals use candidate indices `[1,257)`, with
`coordinate_space:"scalar-stride-index-v1"`. Matches expose `candidate_index`
separately from `scalar`. Stride, range endpoints and block width are hexadecimal;
block width counts candidates for these jobs. See [stride contracts](C23_STRIDES.md).

These `.ndjson` files contain start, batch and summary records. They are volatile
output, not restart checkpoints. `gpu-smoke` validates a diagnostic launch and
explicitly reports no search coverage. Use the next steps for durable work.

## Create and claim durable jobs

These commands run on the CPU even in a GPU build. Python extracts identifiers
from JSON so no example UUID or grant needs to be copied manually. Each job has
one block: this keeps the random claim reproducible while exercising the real
selection option. Larger jobs may use `random-window` or manual block selection;
see [claim policies](STORAGE.md#local-commands). The manifest's width and canonical
inputs are immutable. Use [reference calibration](MULTI_GPU.md#work-units-and-reference-calibration)
for xpoint/BSGS. HASH160, Ethereum, vanity and minikeys currently use explicit block widths; this correctness
slice makes no throughput or calibrated-width claim.

<!-- example: create -->
```bash
"$KEYHUNT_BIN" state project-create --name "GPU quickstart" > "$example_dir/project.json"
PROJECT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$example_dir/project.json")"
for mode in xpoint bsgs hash160 ethereum vanity minikeys22 minikeys30 stride-xpoint stride-hash160 stride-ethereum stride-vanity; do
  job_mode="$mode"
  if [[ "$mode" = stride-* ]]; then
    job_mode="${mode#stride-}"
    targets="$example_dir/$job_mode.txt"; bounds=1:301; width=100; policy=sequential
    table_options=(--stride 3)
  elif [ "$mode" = xpoint ]; then
    targets="$example_dir/xpoint.txt"; bounds=1:101; width=100; policy=sequential
    table_options=()
  elif [ "$mode" = hash160 ]; then
    targets="$example_dir/hash160.txt"; bounds=1:101; width=100; policy=sequential
    table_options=(--encoding both)
  elif [ "$mode" = vanity ]; then
    targets="$example_dir/vanity.txt"; bounds=1:101; width=100; policy=sequential
    table_options=(--encoding both)
  elif [ "$mode" = ethereum ]; then
    targets="$example_dir/ethereum.txt"; bounds=1:101; width=100; policy=sequential
    table_options=()
  elif [[ "$mode" = minikeys* ]]; then
    length="${mode#minikeys}"; job_mode=minikeys
    targets="$example_dir/$mode.txt"; width=100; policy=sequential
    bounds="$(python3 -c 'import json,sys; o=int(json.load(open(sys.argv[1]))["ordinal"],16); print(f"{o:x}:{o+256:x}")' "$example_dir/$mode-inspect.json")"
    table_options=(--length "$length" --input-format hash160)
  else
    targets="$example_dir/public-key.txt"; bounds=1:10001; width=10000; policy=random
    table_options=(--table "$example_dir/babies.khb")
  fi
  "$KEYHUNT_BIN" checkpoint create --project "$PROJECT" --mode "$job_mode" \
    --range "$bounds" --block-width "$width" --targets "$targets" \
    "${table_options[@]}" > "$example_dir/$mode-job.json"
  JOB="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$example_dir/$mode-job.json")"
  "$KEYHUNT_BIN" state claim --project "$PROJECT" --job "$JOB" \
    --owner quickstart --request "$mode-claim-001" --policy "$policy" \
    > "$example_dir/$mode-grant.json"
done
"$KEYHUNT_BIN" state check > "$example_dir/preflight.json"
```

Retain each complete grant response. It carries the original owner, epoch,
generation, block and expiry. Retry an uncertain claim with the **same** request
and options. A new request may reserve additional work. These standalone commands
do not allocate remote coordinator work.

## Checkpoint, inspect and retry

<!-- example: durable -->
```bash
for mode in xpoint bsgs hash160 ethereum vanity minikeys22 minikeys30 stride-xpoint stride-hash160 stride-ethereum stride-vanity; do
  JOB="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["job"])' "$example_dir/$mode-job.json")"
  GRANT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["assignments"][0]["grant"])' "$example_dir/$mode-grant.json")"
  if [[ "$mode" = stride-* ]]; then
    # Recover the step from the immutable job; no stride flag is required here.
    run_options=(--targets "$example_dir/${mode#stride-}.txt" --batch-size 256)
  elif [ "$mode" = xpoint ]; then
    run_options=(--targets "$example_dir/xpoint.txt" --batch-size 256)
  elif [ "$mode" = hash160 ]; then
    run_options=(--targets "$example_dir/hash160.txt" --encoding both --batch-size 256)
  elif [ "$mode" = vanity ]; then
    run_options=(--targets "$example_dir/vanity.txt" --encoding both --batch-size 256)
  elif [ "$mode" = ethereum ]; then
    run_options=(--targets "$example_dir/ethereum.txt" --batch-size 256)
  elif [[ "$mode" = minikeys* ]]; then
    run_options=(--targets "$example_dir/$mode.txt" --length "${mode#minikeys}"
                 --input-format hash160 --batch-size 256)
  else
    run_options=(--targets "$example_dir/public-key.txt" --table "$example_dir/babies.khb"
                 --giant-batch 256 --target-batch 1)
  fi
  "$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
    --grant "$GRANT" "${run_options[@]}" > "$example_dir/$mode-durable.ndjson"
  # The same completed grant must return without launching more search batches.
  "$KEYHUNT_BIN" checkpoint run --backend "$GPU_BACKEND" --device "$GPU_DEVICE" \
    --grant "$GRANT" "${run_options[@]}" > "$example_dir/$mode-retry.ndjson"
  "$KEYHUNT_BIN" checkpoint results --project "$PROJECT" --job "$JOB" \
    > "$example_dir/$mode-results.json"
  "$KEYHUNT_BIN" state block --project "$PROJECT" --job "$JOB" --block 0 \
    > "$example_dir/$mode-block.json"
done
"$KEYHUNT_BIN" state check > "$example_dir/check.json"
```

Each final durable summary reports `complete:true` and `durability:"local"`.
The xpoint/BSGS/Ethereum results files contain one scalar-1 match; HASH160 contains two
encoding relations; vanity contains three prefix/encoding relations. Each block is `finished`, and each retry summary reports zero `batches` with the whole range in `resumed_scalars`. Each minikey job finds two encoding relations and uses `resumed_ordinals`/`computed_ordinals` with `coordinate_space:"minikey-ordinal-v1"`.
Strided retries use `resumed_candidates`/`computed_candidates` and retain the original scalar range and stride.
Completion means that assigned block; it does not mean every block of a larger job.
Local durability does not mean that a coordinator has acknowledged the result.

After interruption, inspect the state and rerun with the retained valid grant and
canonical targets/table. The owner computes the committed complement; work after
the last committed checkpoint can repeat. Stop the old owner before handoff.
For expired or transferred ownership, use [recovery](CHECKPOINTS.md#public-commands)
and [pause/stop controls](PAUSE_RESUME.md), not a fresh unchecked claim. A restored
snapshot is quarantined and cannot simply resume old grants.

These tiny jobs usually finish before a pause can be observed. On longer runs,
use `checkpoint pause|status|resume|stop --state-dir DIR` in another terminal and
wait for `durably_paused:true`. Supervised owners additionally need `--slot QUEUE`.
For authenticated concurrent execution, use the
[localhost worker setup](COORDINATOR.md#s06-isolated-localhost-operation) and
[multi-GPU operations](MULTI_GPU.md). For a disconnected worker use
[C22 manual file exchange](OFFLINE_ASSIGNMENTS.md). Both transports retain the
bounded outbox and stop execution when saved lease deadlines expire.

The example files remain in the printed directory for inspection. They contain
public fixtures and temporary local state; remove that directory when finished.

## Check the documented commands

The harness reads the marked Bash blocks above directly; it does not keep another
copy of the commands. CPU CI runs table preparation, canonical job creation and
both claim policies. Hardware runs additionally check discovery, launches, all
thirteen volatile commands and all eleven durable searches/retries:

```sh
python3 tests/integration/gpu_examples.py --binary build/cpu-release/keyhunt \
  --backend cpu --report /var/tmp/keyhunt-examples-cpu.json
python3 tests/integration/gpu_examples.py --binary build/hip-release/keyhunt \
  --backend hip --device 0 --report /var/tmp/keyhunt-examples-hip.json
# On the NVIDIA host:
python3 tests/integration/gpu_examples.py --binary build/cuda-h200/keyhunt \
  --backend cuda --device 0 --report /var/tmp/keyhunt-examples-cuda.json
```

The harness checks fixed expected results/coverage, records the exact document and
binary hashes, and removes only its own temporary example directory. These are
command checks, not throughput measurements or replacements for the backend's
full oracle, corruption and recovery suites.
