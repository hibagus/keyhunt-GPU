# Usage

Run these commands from the repository root after following [BUILD.md](BUILD.md).
The legacy `-m` search commands use the preserved CPU engine. HIP provides device
discovery, bounded diagnostics and the separate [bounded xpoint search](HIP_XPOINT.md).
C10 adds [versioned BSGS table preparation](BSGS_TABLES.md), including HIP lookup
validation, and C11 implements [bounded HIP BSGS range search](HIP_BSGS.md).
C12/C13 add [local state](STORAGE.md) and [verified checkpoint commands](CHECKPOINTS.md).
Authenticated coordinator commands remain planned.

## A finite address search

```sh
./build/cpu-release/keyhunt -m address -f tests/1to32.txt \
  -r 1:401 -n 1024 -l compress -t 1 -q -s 0
```

`-r` bounds are hexadecimal; this aligned range covers scalars 1 through 1024.
`-n 1024` selects the original CPU batch size. `-l compress` selects compressed
Bitcoin addresses, `-t 1` uses one CPU thread, `-q` suppresses batch-base output,
and `-s 0` disables periodic statistics. The known scalar-1 match is included.
The small range finishes without an external timeout.

Unaligned ranges can overshoot the end, and stride batches can overlap. Use the
[CPU baseline](CPU_BASELINE.md#range-endpoints-and-selection) to understand these
existing defects. C05 supplies a separate [exact half-open planner](EXACT_RANGES.md);
it has not replaced the legacy CPU loop.

## A finite BSGS example

Create a temporary working directory so this example's result file stays separate
from other searches. The public key is a synthetic fixture for scalar `0x100001`.

```sh
keyhunt_bin="$PWD/build/cpu-release/keyhunt"
example_dir="$(mktemp -d)"
printf '%s\n' '039fb8987e3cb30a1174b7d64de26347166841051854696930396502714f6bcf4b' \
  > "$example_dir/target.pub"
(cd "$example_dir" && "$keyhunt_bin" -m bsgs -f target.pub \
  -r 100000:300000 -n 1048576 -t 1 -q -s 0)
```

The expected output contains `Key found privkey 100001`. The original program
returns **1** when every BSGS target is found; check the output/result file when
interpreting that status. Ordinary range exhaustion returns 0. Its help command
also returns 1. The example uses the minimum supported square-root group size
(M=1024) and default `-k 1`, avoiding the large default table.

C06 fixed the tested start-boundary miss; the BSGS tail-overrun defect remains.
This fixture tests an interior
match and is not a demonstration of exhaustive coverage. See
[the BSGS findings](CPU_BASELINE.md#bsgs-boundary-defects). The historical README
contains larger examples, but their old performance numbers are not current
benchmarks.

## Input and output

Choose a [mode](MODES.md) matching the target file: addresses, HASH160 values,
x coordinates or public keys are different formats. Existing inputs under
`tests/` retain their original paths. Synthetic regression vectors are in
`tests/baseline/vectors.json`.

Results are appended in the process working directory:

| File | Purpose |
| --- | --- |
| `KEYFOUNDKEYFOUND.txt` | Ordinary and BSGS matches |
| `VANITYKEYFOUND.txt` | Vanity matches |
| `keyhunt_bsgs_*.blm`, `keyhunt_bsgs_*.tbl` | Precomputation caches when enabled with `-S` |

Caches speed up table preparation; they do not record visited ranges. Empty or
malformed inputs are not consistently rejected by the old parser. New structured
job input validation will be added during core extraction.

## Search controls today

`-R` repeatedly chooses random bases and can revisit work. `-n` is a batch/table
parameter, not a global limit. Minikey search uses its own candidate domain and
continues after finding matches; scalar `-r` bounds do not make it finite.

For the ordinary legacy/C09/C11 commands, Ctrl-C stops the process without a
durable progress checkpoint. An OS stop/continue can suspend and resume that same
live process, but its memory does not survive failure. Graceful checkpoint-on-signal
controls remain C14 work. C12 provides local assignment ownership and thirty-day
expiry through [`keyhunt state`](STORAGE.md). C13 adds the separate
[`keyhunt checkpoint` commands](CHECKPOINTS.md), which persist verified HIP results
and accepted coverage and resume the committed complement after process failure.
The ordinary legacy and C09/C11 search commands retain their volatile behavior.

`bsgsd` is the old local table daemon, not the authenticated coordinator. Its
[protocol document](../BSGSD.md) remains available; C02 documents that its port
option is not honored by the socket bind. Keep it on loopback.

## Reproducible checks

```sh
python3 tests/baseline/run_cpu_baseline.py \
  --binary build/cpu-release/keyhunt --report /tmp/keyhunt-cpu-results.json
python3 tools/capture_environment.py \
  --binary build/cpu-release/keyhunt --output /tmp/keyhunt-environment.json
python3 tools/benchmark_cpu_baseline.py \
  --binary build/cpu-release/keyhunt --report /tmp/keyhunt-cpu-benchmark.json
```

Test execution uses temporary directories and public synthetic keys. The short
benchmark measures process wall time including setup and shutdown polling; it
is not a GPU benchmark or a precise arithmetic throughput measurement.

The [historical README](HISTORICAL_README.md) preserves the previous lengthy mode
examples, original acknowledgments and historical speed discussions. Consult
current validation records before relying on those older claims.

## HIP discovery and launch check

```sh
./build/hip-release/keyhunt devices --backend hip
./build/hip-release/keyhunt gpu-smoke --backend hip --device 0 --steps 257 \
  --start 0x100000000ffffffffffffffff
```

Both commands print JSON and return 0 on success, 2 on invalid input or backend
failure. Discovery can succeed with an empty device list; a launch requires a
visible device. Device ordinals honor the runtime's visibility settings. The
launch checks exact scalar indices on the GPU and verifies them on the host;
its output explicitly reports that it provides no search coverage. No target
file or result file is involved. CPU-only builds reject both HIP requests.
See [the HIP contract](HIP_BACKEND.md) for bounds, timings and limitations.
