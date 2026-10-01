# Bounded HIP BSGS search (C11)

## Mapping and targets

C11 consumes C10's validated [versioned baby table](BSGS_TABLES.md). For a
nonempty scalar tile `[a,b)`, baby entries represent `jG`, `0 <= j < m`.
For each canonical target `Q`, the search looks up `Q - aG - i*mG` for
`0 <= i < ceil((b-a)/m)`. A full-point match reconstructs `k = a + i*m + j`.
Only `a <= k < b` is eligible, and every emitted match must satisfy `kG == Q`
in the preserved CPU verifier. Absolute scalars, endpoints and reconstruction
products use checked 256-bit integer arithmetic, never scalar-field reduction.

The last giant step admits only `(b-a) mod m` babies when this is nonzero.
Infinity is a valid residual and matches baby zero. Both point signs are
preserved through full compressed-point lookup; matching an X coordinate alone
is insufficient. Every exact collision-list entry must be considered.

Target files contain finite compressed (66 hex digits) or uncompressed (130 hex
digits) SEC1 public keys, one per nonempty line. LF, CRLF and no final newline
are accepted. Coordinates must be canonical and on curve; infinity, hybrid
encodings, nonresidues, embedded NUL and overlong lines are rejected. Parsing is
bounded to 65,536 input points. Compressed and uncompressed duplicates normalize
to one uncompressed point; opposite signs remain distinct. Sorted full points
are hashed with the `bsgs-targets-v1` domain tag, fixing canonical target IDs.

`BsgsBatch` binds an interval and contiguous target subset to the complete target
digest and table checksum. A batch permits at most 64 targets and 1,048,576
*target giant steps* (`giants * target_count`). The table size remains fixed
across batches. `bsgs_tile` clips a checked `m * max_giants` width before adding
it to the start, or subtracting it from the end for reverse traversal, avoiding
overflow at the order endpoint. This separate BSGS
plan does not reuse the scheduler's `DirectXPointV1` scalar mapping.

The owner must finish every target subset before crediting a tile once. A match
does not imply that other targets or the rest of the interval have been searched.
The ordinary C11 command emits volatile receipts. The separate C13
[checkpoint commands](CHECKPOINTS.md) provide verified durable execution using
the same HIP kernels and the C12 local journal.

C23 adds execution-only `--tile-order forward|reverse|both-ends` to HIP/CUDA native searches,
checkpoint runs and workers. Reverse selects scalar tiles from the upper end;
GPU arithmetic inside each tile remains forward. It can change on restart without
changing job identity. See [contract and example](C23_BSGS_REVERSE.md).

Both-ends starts low and alternates per completed tile, including inside adaptive
work units. Restart begins low again on remaining coverage. See
[the both-ends contract and example](C23_BSGS_BOTH_ENDS.md).

## Initial host validation

The CPU contract test passes in release and focused ASan/UBSan builds. It checks
SEC1 normalization and malformed input, opposite signs, target identity, candidate
verification/corruption, final partial tiles, products above 64 bits and an
exclusive endpoint at the curve order. GPU execution, independent oracle cases
and performance evidence are recorded below as their acceptance gates complete.

## Command and execution contract

Build a compatible cache once, then reuse it across search tiles:

```sh
./build/cpu-release/keyhunt bsgs-table build --m 65537 --output /tmp/babies.khb
printf '%s\n' 0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798 > /tmp/points.txt
./build/hip-release/keyhunt bsgs --backend hip --table /tmp/babies.khb \
  --targets /tmp/points.txt --range 1:100000 --device 0
```

Endpoints are hexadecimal and the end is exclusive. These commands are separate
from the legacy CPU `-m bsgs` interface and its cache formats. A CPU-only build
rejects `--backend hip` explicitly. There is no automatic CPU fallback or
stop-on-match behavior. Only finite full public keys are accepted as BSGS targets.

| Option | Default | Contract |
| --- | ---: | --- |
| `--giant-batch` | 16384 | Maximum giants per target per tile; at most 1048576 |
| `--target-batch` | 64 | Maximum targets per launch, 1..64; product with giant batch at most 1048576 |
| `--candidate-capacity` | 1024 | Bounded records, 1..65536; overflow requires replay |
| `--group-size` | auto | Automatic selection or explicit 1/8 giants per lane |
| `--tile-order` | forward | `forward` selects the lowest remaining tile; `reverse` selects the highest; `both-ends` alternates low/high |
| `--host-memory` | 1073741824 | Checked table decode/preparation/search buffer budget in bytes |
| `--reserve-bytes` | 67108864 | Keep this many currently free bytes unused on the selected HIP device |
| `--device` | 0 | Ordinal within the runtime's current visible logical devices |

The NDJSON stream contains:

- `start`: exact interval, canonical target digest/count, table checksum/m,
  device UUID, requested group policy (`0` means auto), `tile_order`, preparation/upload time.
- `batch`: a target subset over the tile, actual dispatched group, device and
  verified **target giant steps**, candidate count, tail rejections, CPU-verified
  matches and timings/allocation sizes. Overflow emits no consumable matches and
  zero verified steps, even when the bounded prefix contains valid hits.
- `tile`: emitted only after every target subset completed, with the scalar
  interval credited once and `durable_coverage: false`.
- `summary`: emitted after the whole requested interval completed. Scalar-range
  coverage, target giant steps and replayed device steps are separate counters;
  `tile_order` reports the selected traversal.

Target IDs refer to the sorted canonical uncompressed point set. Batch matches
are sorted by scalar; the entire stream follows target-subset order within each
tile, not global scalar order. A failed output write prevents further submission.
Flushing NDJSON is backpressure, not durable match storage or checkpointing.

The executor owns one nonblocking stream, immutable uploaded targets, twenty
cached `-(m*2^bit)G` points and one prepared C10 table. It derives `aG` once per
submitted tile/subset on the CPU; lanes form `Q-aG-i*mG`, walk by `-mG`, normalize
both X and Y and apply the shared Bloom/exact lookup. The eight-step variant
shares one inversion, preserving zero Z values as infinity. Every equal-key
entry is traversed, and the final giant clips babies before candidate emission.

One result slot prevents reuse before `take()`. Tickets share the existing global
executor namespace, rejecting foreign or stale owners/sequences. Runtime or
verification failures poison the executor. Cleanup drains its stream before
freeing allocations and before destroying the prepared table. The caller retains
the host table and CPU verifier, without moving/reinitializing them, until the
executor is destroyed. CPU curve initialization precedes worker threads.

Every runtime allocation checks its result. Search memory is checked against
free bytes on the selected logical device after table upload; no package-HBM or
partition multiplier is used. Host checks conservatively include table peak
buffers, target copies/staging, bounded candidates/results and pinned memory;
they are allocation planning budgets, not an OS-enforced process RSS limit.

Because v1 contains every unique `jG` for `j < m < n`, each canonical full target
has at most one in-range scalar. C17 therefore caps the output allocation at
the minimum of requested capacity, executor steps, unique targets and 64 (the
maximum batch subset), plus the guard. On overflow the CLI retries the same target
cursor with `min(capacity, old_count/2)` targets. This terminates even at capacity
one, without reducing m or crediting a partial attempt. Successful target subsets
remain volatile; an interrupted process must restart its requested interval.

## Kernel selection

C17 adds a shorter inversion chain and mixed point addition for group 1; group 8
retains general additions because its measured mixed variant regressed.
[Paired tuning evidence](HIP_TUNING.md) records accepted and rejected variants.

All launches use 128 threads. The matching `__launch_bounds__(128)` declaration
allows the compiler to allocate registers for the actual block size. The
unbounded declaration's resource report showed unnecessary VGPR spills; the
bounded version removes those VGPR spills and reduces compiler-allocated LDS.
Scratch storage and SGPR spills remain; this is not a claim of a spill-free search.

Grouping lowers inversion cost but also reduces parallel lanes. Automatic mode
uses eight steps when `4 * grouped_blocks * target_count >= visible_compute_units`,
otherwise one. This crossover comes from the measured one-, three- and 32-target
workloads, not a universal optimum. It uses the runtime's actual logical-device
CU count, preserving CPX/QPX/SPX discovery behavior. Explicit group overrides
support further comparisons without changing the mathematical search contract.

```sh
./build/hip-release/hip_bsgs_search_benchmark 0 65537 32768 > /tmp/bsgs-search-large.json
./build/hip-release/hip_bsgs_search_benchmark 0 257 32768 > /tmp/bsgs-search-small.json
```

The benchmark fixes table m, targets and scalar tile, validates every expected
match, warms each owner once, then measures five samples with rotating dispatch
order. It compares group 1, group 8 and automatic mode. Build/upload costs are
reported separately from warm submit-to-take wall time, which includes seed
construction, transfers and CPU verification. It does not include durable
storage, multi-device scheduling or checkpoint costs.


## Measured search results

On the current SPX/NPS1 MI300X, each benchmark tile contains 32,768 giants per
target. These are medians of five warm samples in automatic mode. Scalar coverage
credits the interval once after **all** targets; it is not a count of public-key
evaluations. Rates use wall time including transfers and CPU verification.

| m | Targets / workload | Group | Kernel ms | Wall ms | Target giant steps/s | Scalar-range coverage/s |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 65,537 | no_match_1 | 1 | 0.629870 | 0.673315 | 4.867e+07 | 3.189e+12 |
| 65,537 | boundary_3 | 8 | 0.765017 | 0.818000 | 1.202e+08 | 2.625e+12 |
| 65,537 | no_match_32 | 8 | 1.214317 | 1.258633 | 8.331e+08 | 1.706e+12 |
| 257 | no_match_1 | 1 | 0.629990 | 0.673435 | 4.866e+07 | 1.251e+10 |
| 257 | boundary_3 | 8 | 0.766219 | 0.824611 | 1.192e+08 | 1.021e+10 |
| 257 | no_match_32 | 8 | 1.150452 | 1.196085 | 8.767e+08 | 7.041e+09 |

At m=65,537, explicit group 8 improves the 32-target median wall time by 5.29x
against explicit group 1. Automatic mode retains the faster single-lane layout
for the one-target workload. This does not establish an optimal crossover for
all table sizes, workloads or partition modes. C16/C17 retain the broader tuning
gates, including pressure from larger tables and different target distributions.

With the 128-thread launch bound, compiler reports show 248 VGPRs and zero VGPR
spills in both variants, versus 128 VGPRs and spills in the initial unbounded
declaration. Both still use 106 SGPRs; reported SGPR spills are 188/248 for group
1/8, scratch is 208/1232 bytes per lane, and compiler-allocated LDS is 4096 bytes
per block. Estimated occupancy is two waves/SIMD. These are compiler reports,
not measured occupancy; remaining scratch/register pressure is documented tuning
work, not hidden behind the throughput improvement.

[Raw measurements](baselines/C11_MEASUREMENTS.json) preserve warm-ups, five measured
samples, min/median/max statistics, initial launch-bound comparison, source/binary
fingerprints, input/target/table digests, compile commands and resource remarks,
runtime/driver/tool versions, UUID/topology, CPU affinity and clock/power snapshots.
Only this logical device was benchmarked; eight-device correctness is a separate
gate. The host was unreserved and no operating settings were changed.


## C11 acceptance

[Search evidence](baselines/C11_SEARCH.json) records 95 independent cases for
each of automatic, group-1 and group-8 execution (285 runs), using 3,885 pinned
libsecp256k1 public keys per suite. Cases exhaust every tiny subinterval, test
seeded high offsets and 32/64/128/192/255-bit carries, both signs/infinity,
partial giants, all twenty local-offset bits, no-match jobs, target groups beyond
64 entries, capacity-one replay and all eight visible devices. Each mode also
passes 37 invalid-input/device/budget/output rejection checks.

The executor tests cover owned results, stale/foreign tickets, busy slots,
identity binding, destruction with pending work and all-target replay. The
separate fault binary passes 35 constructor, runtime and downloaded-result
corruption boundaries, rejecting reuse after a failure and recovering with a
fresh executor. C10's independent cache/filter/collision tests remain in the gate.

[Final acceptance evidence](baselines/C11_VALIDATION.json) records 37/37 HIP release, 22/22 CPU release, 22/22 CPU debug and
20/20 focused ASan/UBSan tests passing. The sanitizer gate excludes the previously
documented `cpu_baseline` and `target_loading` legacy tests; whole-application
sanitizer cleanliness is not claimed. Actual C11 execution currently covers
SPX/NPS1; CPX/QPX/SPX discovery contracts pass without partition changes, while
CPX/QPX hardware search runs remain pending.

C11–C13 are complete. C12 provides [sparse persistent coverage and transactional
assignments](STORAGE.md); C13 binds verified matches and coverage in
[durable local checkpoints](CHECKPOINTS.md).
