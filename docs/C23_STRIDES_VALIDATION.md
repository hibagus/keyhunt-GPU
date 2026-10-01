# C23 exact positive scalar stride acceptance

The selected scope adds positive strides to xpoint, Bitcoin HASH160/P2PKH,
Ethereum and Bitcoin vanity on HIP and CUDA, with exact candidate coverage,
durable recovery and online/offline workers. Endomorphism and alternative search
mappings remain pending; C23 as a whole remains partial.

## Behavior and decisions

`--range A:B --stride S` visits exactly `A+i*S<B`. Hexadecimal inputs must satisfy
`1<=A<B<=n` and `1<=S<n`. There is no wrap or swapped endpoint. Stride one retains
the existing scalar coordinates, 50-byte configuration and job identity. For
nonunit strides, candidate index `j` maps to `A+(j-1)*S`; blocks and coverage use
`[1,N+1)`, where `N=1+floor((B-A-1)/S)`. Block width counts visited candidates.

The [contract](C23_STRIDES.md) explains the 146-byte version-2 configuration,
which binds original bounds and stride to the immutable job. Target formats,
schema 7 and receipt encoding remain unchanged. Historical receipt fields named
`scalar` store candidate indices; public results show both `candidate_index` and
the actual scalar. Durable summaries report candidate counts explicitly.

Direct kernels use checked multiply-add arithmetic. Stepped kernels seed each
batch at its mapped scalar and cache powers of `SG`. Modular point-cache
arithmetic does not change the nonwrapping candidate progression. Prepared
executors reject a mismatched stride. Existing per-family buffer bounds and
whole-attempt overflow rejection still apply.

Checkpoint runs infer the persisted mapping; an explicit mismatched stride fails
before execution. Workers require `scalar-stride-v1` before acquiring, renewing
or uploading strided work. Earlier capability sets remain accepted for their
supported version-1 jobs. Fresh runtime self-tests cover both kernels and all
four scalar families on the owned ordinal. BSGS and minikey enumeration do not
accept this scalar-stride option.

## Validation evidence

The [manifest](baselines/C23_STRIDES_VALIDATION.json) records source phases,
binary hashes, compiler settings, device inventories and artifact checksums.
Algorithm, storage and worker gates ran in phases; earlier search binaries are
identified separately from the final integrated binaries.

| Gate | Coverage |
| --- | --- |
| CPU release | 81 tests; the corrected canonical-API fixture passes its separate rerun |
| ASan/UBSan | Five new mapping, arithmetic, storage and coordinator gates; focused API boundary rerun |
| Device arithmetic | 2,509 independent Python-integer vectors per portable/native implementation |
| Native searches | 104 cases and 24 invalid-stride rejections per backend; direct/stepped kernels and all eight visible ordinals |
| Boundaries | Wide origins and strides, curve order, off-lattice/no-hit targets, overlapping relations, non-divisible tails and maximum 1,048,576-candidate batches |
| Executor regression | Four existing ownership/fault suites per backend plus immutable stride/cache binding checks |
| Durable recovery | 24 direct/stepped cases and four killed-process restarts per backend, checked against independent target relations |
| Pause/restart | Four families, repeated pauses, graceful signals, backup/restore quarantine and changed device visibility |
| Coordinator | Old-worker fences before mutation/cached reply, altered mapping import, lost upload response and duplicate replay |
| Transports | Eight live HTTPS/file cases per backend; two blocks with one executor setup, exact local/server result reconciliation and empty acknowledged outbox |
| Regression | Existing HTTPS/offline family suites and CUDA primary-context isolation |
| Documentation | CPU/HIP/CUDA execute 13 volatile commands, 11 durable jobs and completed-grant retries from the marked quickstart |

[Algorithm reports](baselines/C23_STRIDES_ALGORITHMS.json),
[recovery reports](baselines/C23_STRIDES_RECOVERY.json),
[worker reports](baselines/C23_STRIDES_WORKERS.json),
[executable examples](baselines/C23_STRIDES_EXAMPLES.json) and
[raw logs](baselines/C23_STRIDES_LOGS.tar.gz) retain detailed observations. Fixtures
use independently generated public points and synthetic ranges. The archive
excludes credentials and journal databases.

The initial live API fixture omitted the required canonical `0x` prefix and was
correctly rejected. Its corrected rerun passes. Review then identified that a
malformed submitted mapping was reported as temporary coordinator unavailability;
job creation now returns HTTP 400 before persistence. Decoding stored bindings
retains its corruption behavior. Focused CPU, sanitizer, HIP and CUDA checks
cover the final API boundary independently of the earlier integrated gates. A
final explicit empty optional initializer removes a new compiler warning; focused
storage/coordinator checks pass after that cleanup.

## Reproduction and limits

Build using [BUILD.md](BUILD.md), then run
`ctest --test-dir BUILD -R stride --output-on-failure`. The CLI/oracle tests also
require the existing [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits).
Run the earlier executor/fault, HTTPS/offline and CUDA context checks listed in
the manifest. The [quickstart](GPU_QUICKSTART.md) includes a small strided example
for every scalar family and the Bitcoin address alias.

Validation uses eight SPX/NPS1 MI300X devices and eight MIG-disabled H200s.
Sequential ordinal checks do not establish concurrent scaling. There is no new
throughput, tuning, calibrated block-width, partition/MIG certification or public
coordinator deployment claim. Existing calibration remains for its original
xpoint/BSGS mappings. Legacy `-I` and `-m` paths retain their characterized CPU
behavior. Negative/zero strides, BSGS strides, minikey ordinal strides,
endomorphism and alternative traversal require separate contracts and evidence.
