# C23 exact-range GLV acceptance

The selected scope adds opt-in `--kernel glv` to xpoint, Bitcoin HASH160/P2PKH,
Ethereum and Bitcoin vanity. Portable arithmetic, native HIP/CUDA searches,
checkpoint recovery and HTTPS/offline owners passed the recorded gates.
The default remains `stepped`. Legacy related-key orbit expansion, BSGS/minikey
GLV and other search mappings remain separate work; C23 overall stays partial.

## Behavior and implementation decisions

GLV computes exactly `kG` for each original candidate scalar k. It does not
search additional keys. Signed 128-bit components satisfy
`k = k1 + lambda*k2 mod n`; joint multiplication uses G and
`phi(G)=(beta*G.x,G.y)`. The [contract](C23_GLV.md) records provenance,
integer/field separation, compatibility and fail-closed bounds.

Production uses portable 32-bit limbs, full 512-bit reciprocal products and
checked signed lattice residuals. It never links the pinned test oracle. The
shared direct-search kernels select GLV arithmetic at compile time while keeping
checked scalar mappings, target lookup, candidate guards and counters. CPU
verification remains authoritative for every returned relation. Overflow
certifies no part of an attempt; replay retains the existing exact-range rules.

Kernel choice is execution-only. Unit-forward scalar coordinates, positive
stride indices and reverse indices retain their configuration versions 1/2/3,
job identities, receipts and schema 7. A restart can switch direct/GLV/stepped
without a migration. Workers retain their mapping capability requirements and
self-test all three scalar kernels on the owned device before execution. GLV
retains uploaded targets across grants but needs no stepped point-power cache.
BSGS rejects scalar kernel overrides; minikeys still accepts only direct.

## Validation evidence

The hardware gates use eight visible SPX/NPS1 MI300X devices and eight
MIG-disabled H200s. This establishes correctness on the recorded stacks and
ordinals, without a fleet scaling or partition certification claim.

| Gate | Coverage |
| --- | --- |
| CPU release | All 96 tests passed at the durable integration phase |
| ASan/UBSan | Portable GLV probe: all 5,973 arithmetic/oracle cases |
| Arithmetic | 5,328 decomposition inputs, 603 full public keys, 36 endomorphism/alias cases and six invalid-public-key cases per implementation |
| Boundaries | All four sign combinations, reciprocal rounding boundaries, powers/carries, wide random values and curve-order endpoints |
| Native searches | 144 independent cases and 72 invalid mapping rejections per backend; four families and all eight ordinals |
| Mapping/targets | Forward/reverse, unit/nonunit strides, short tails, maximum 1,048,576-candidate batches, miss targets, both Bitcoin encodings and overlapping vanity prefixes |
| Executor failures | Three kernel choices retain every-index/ownership/overflow gates; injected runtime and downloaded-data failures poison the owner |
| Checkpoints | 36 cases per backend, including eight killed-process restarts; exact independent results and committed coverage survive kernel switches |
| Pause/visibility | Eight four-family cases per backend; GLV → direct → stepped, repeated pause/signals, changed visibility, backup and restore quarantine |
| Transports | 16 HTTPS/file cases per backend; two grants with one executor setup, exact local/server results, retry and offline execution |
| Regression | CPU suite, direct/stepped executor paths, HTTPS/offline workers and CUDA context isolation |
| Examples | CPU preparation and 23 volatile/19 durable searches plus completed-grant retries on HIP and CUDA |
| Measurement | 20 paired workloads and 540 raw measured samples per backend, with no timing pass threshold |

The [manifest](baselines/C23_GLV_VALIDATION.json) records source phases, binary
hashes, compiler/build settings, inventories and artifact checksums.
[Algorithms](baselines/C23_GLV_ALGORITHMS.json),
[recovery](baselines/C23_GLV_RECOVERY.json),
[workers](baselines/C23_GLV_WORKERS.json),
[examples](baselines/C23_GLV_EXAMPLES.json),
[performance](baselines/C23_GLV_PERFORMANCE.json) and
[raw logs](baselines/C23_GLV_LOGS.tar.gz) preserve the detailed observations.
Archives contain no enrollment credentials or journal databases.

The first HIP reverse CLI run exposed a fixture error: a supposed negative
unit-stride target was inside the interval, but absent from the sparse expected
set. The kernel correctly returned it. Moving that target outside the interval
fixed the expectation; both complete CLI matrices then passed. The initial
failure and successful reruns are retained. Existing explicit-constructor
warnings in `storage/checkpoint.h` remain outside this slice.

Algorithm and integrated CUDA binaries use separate immutable checkouts. Search
and recovery ran concurrently after their own builds; benchmarks ran after all
correctness/example work finished. Focused sanitizer results cover the portable
GLV probe, not a full application sanitizer run. CPU release evidence precedes
the expanded quickstart; its final commands were executed separately on all
three build types.

## Measured performance and decision

Each workload uses 65,536 candidates, two boundary targets per encoding, two
warmup rounds and nine measured rounds for each kernel. Rotating execution order
balances which kernel runs first. Preparation, event timing, submit-to-take wall
time, transfers, CPU verification and allocations are recorded. All samples
check exact coverage and boundary matches. Regions include low scalars, sparse
values around bits 128/192, a dense scalar region and values immediately below n.

The table shows the range across the four families of **median kernel-event
speedup**: reference milliseconds divided by GLV milliseconds. Values below 1
mean GLV is slower. It does not summarize wall time or durable throughput.

| Device | Region | GLV versus direct | GLV versus stepped |
| --- | --- | --- | --- |
| MI300X | `low` | 1.02–1.05x | 0.34–1.87x |
| MI300X | `bit128` | 0.58–0.65x | 0.07–0.68x |
| MI300X | `bit192` | 0.72–0.77x | 0.07–0.63x |
| MI300X | `dense256` | 1.39–1.47x | 0.07–0.67x |
| MI300X | `order` | 4.34–8.03x | 0.34–1.86x |
| H200 | `low` | 1.00–1.01x | 0.12–0.50x |
| H200 | `bit128` | 0.50–0.54x | 0.03–0.13x |
| H200 | `bit192` | 0.59–0.65x | 0.03–0.11x |
| H200 | `dense256` | 1.32–1.40x | 0.03–0.13x |
| H200 | `order` | 6.28–6.84x | 0.13–0.51x |

Performance depends on scalar bit pattern and target/hash workload. Dense inputs
benefit relative to direct multiplication, while sparse inputs can regress;
near-order values often split into very small signed components. Stepped remains
a strong baseline and faster on the dense region across the measured families.
Keep GLV opt-in. These results do not justify automatic kernel selection,
changing the default, or a universal speedup claim.

## Reproduction and remaining scope

Build using [BUILD.md](BUILD.md), set the existing
[pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits),
and run `ctest --test-dir BUILD -R glv --output-on-failure`. Run the four-family
executor/fault gates, stride/reverse executor checks, `coordinator_https_worker`,
`coordinator_offline_cli` and CUDA's `coordinator_cuda_contexts`. Execute the
marked [quickstart](GPU_QUICKSTART.md) with its harness. The
[paired benchmark commands](BUILD.md#optional-glv-scalar-kernels) record both
raw samples and summary medians; run them on an otherwise idle device.

The fixtures use independently derived public data and synthetic ranges.
No related-key orbit expansion, BSGS/minikey GLV kernel, additional scalar order,
new calibration, fleet performance, public deployment or schema migration is
included. Legacy CPU flags retain their existing semantics.
