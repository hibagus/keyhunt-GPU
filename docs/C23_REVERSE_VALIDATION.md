# C23 exact reverse traversal acceptance

The selected scope adds `--order reverse` to xpoint, Bitcoin HASH160/P2PKH,
Ethereum and Bitcoin vanity, including unit and positive nonunit strides.
The implementation includes direct/stepped GPU kernels, exact candidate coverage,
durable recovery and online/offline workers, validated on the recorded HIP/CUDA stacks.
Endomorphism and other search mappings remain pending; C23 overall stays partial.

## Behavior and decisions

`--range A:B --stride S --order reverse` visits exactly the forward candidate set
from last to first. With `N=1+floor((B-A-1)/S)`, candidate index j maps to
`A+(N-j)*S`. The first scalar is the final on-lattice value below B, which need
not equal B-1. Hexadecimal bounds/stride satisfy `1<=A<B<=n` and `1<=S<n`.
Scalar arithmetic cannot wrap. Reverse work always covers indices `[1,N+1)`,
including stride one; block widths count candidates.

The [contract](C23_REVERSE.md) defines configuration version 3, with the same
146-byte layout as forward strides. Version 3 implies reverse order and permits
stride one. All bounds, stride and order participate in immutable job identity.
Default or explicit forward order retains version-1/2 semantics. A changed order
requires a new job. Schema 7 and receipt bytes remain unchanged.

Public results show actual `scalar` and `candidate_index` separately with
`coordinate_space:"scalar-reverse-index-v1"`. Durable summaries use
`computed_candidates` and `resumed_candidates`. Generic grant/state intervals
retain their format and inherit the job's coordinate mapping. Historical receipt
fields named `scalar` contain candidate indices for mapped jobs.

Direct kernels use checked multiply-subtract; stepped kernels seed each batch
at its first mapped scalar and advance by `-SG`. Cached powers use
`(n-S)*2^bit mod n`; only point arithmetic is modular. Prepared executors reject
both stride and direction mismatches. Every relation is independently verified
on the CPU, and overflow rejects the entire attempted interval before replay.

Checkpoint runs recover order and stride from saved state; explicit conflicting
flags fail before execution. The coordinator requires the eighth capability
`scalar-reverse-v1` before allocation, renewal, uploaded checkpoints or cached
replies for reverse jobs. Earlier workers retain their supported forward jobs.
Fresh runtime self-tests cover both directions and kernels on the owned device.
BSGS and minikey enumeration reject this scalar order option.

## Validation evidence

Algorithm and recovery gates passed on eight SPX/NPS1 MI300X devices and eight
MIG-disabled H200s. Worker and executable-example gates also passed on both stacks.

| Gate | Coverage |
| --- | --- |
| CPU release | 89 tests |
| ASan/UBSan | Five focused mapping, arithmetic, storage and coordinator gates |
| Host mapping | Tiny exhaustive progressions, full scalar domain, inverse mapping and 3,343 independent oracle requests |
| Device arithmetic | 2,509 Python-integer subtraction vectors per portable/native implementation |
| Native searches | 112 cases and 36 invalid stride/order rejections per backend, both kernels and all eight visible ordinals |
| Boundaries | Unit strides, short tails, wide borrows/strides, curve order, skipped/no-hit targets, overlapping vanity relations and maximum 1,048,576-candidate batches |
| Executor regression | Both direction/cache binding and the four existing ownership/fault suites |
| Durable recovery | 32 direct/stepped cases and four killed-process restarts per backend, checked against independent target relations |
| Pause/restart | Four families, repeated pauses, signals, backup/restore quarantine and changed device visibility |
| Compatibility | Immutable order, malformed bindings, old-worker fences before cached replies/mutation and seven-capability forward workers |
| Transports | Eight live HTTPS/file cases per backend, two blocks with one executor setup and exact local/server relation sets |
| Regression | 104 forward stride searches per backend, forward worker transports, HTTPS/offline suites and CUDA context isolation |
| Documentation | CPU/HIP/CUDA execute 18 volatile searches, 15 durable jobs and completed-grant retries from the marked quickstart |

The [evidence manifest](baselines/C23_REVERSE_VALIDATION.json) records source
phases, binary hashes, compiler settings, inventories and artifact checksums.
[Algorithm reports](baselines/C23_REVERSE_ALGORITHMS.json),
[recovery reports](baselines/C23_REVERSE_RECOVERY.json),
[worker reports](baselines/C23_REVERSE_WORKERS.json),
[executable examples](baselines/C23_REVERSE_EXAMPLES.json) and
[raw logs](baselines/C23_REVERSE_LOGS.tar.gz) retain the detailed observations.
The archive excludes credentials and journal databases. Algorithm and integration
binaries were tested in separate phases; the manifest identifies each one.
HIP's first algorithm run preceded comment/formatting/diagnostic cleanup, with
unchanged search arithmetic. Final HIP worker/examples and CUDA gates use the
committed implementation. Focused sanitizer evidence covers the five listed
test targets, not a full application sanitizer run.

The separate H200 coordinator configure
initially lacked paths to its extracted dependencies. Reusing the established
JSON/curl/SQLite paths fixed configuration without a source change. Both
configuration logs are retained with the evidence.

## Reproduction and limits

Build using [BUILD.md](BUILD.md), then run
`ctest --test-dir BUILD -R reverse --output-on-failure`. Oracle integration uses
the existing [pinned Keccak environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits).
Also run the four-family executor/fault gates, forward stride regressions,
`coordinator_https_worker`, `coordinator_offline_cli` and CUDA's
`coordinator_cuda_contexts`. Execute the marked [quickstart](GPU_QUICKSTART.md)
using its harness.

Fixtures use independent integer/public-key oracles and synthetic ranges.
Sequential ordinal checks do not establish concurrent scaling. This slice adds
no throughput, tuning, calibrated-width, partition/MIG certification or public
coordinator deployment claim. Legacy CPU `-I`/`-m` behavior is unchanged.
Endomorphism, BSGS/minikey traversal and other orders require separate contracts
and parity evidence.
