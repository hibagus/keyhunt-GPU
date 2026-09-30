# C22: offline assignment export and reconciliation

C22 is complete for trusted manual file exchange, localhost mTLS and disconnected
xpoint/BSGS execution on MI300X and H200. The [operator guide](OFFLINE_ASSIGNMENTS.md)
covers configuration, courier commands, recovery and limits.

## Findings and decisions

C22 uses the existing authenticated machine-sync transaction to reserve blocks,
renew valid leases and reconcile CPU-verified checkpoints. A disconnected worker
persists a request; a connected courier authenticates it with the worker's mTLS
credential and returns the response file. The coordinator reserves work before
publishing that response. A worker must import it before execution.

This avoids separate allocation or checkpoint formats. Schema v7 adds a durable
transfer ledger while preserving the published v1–v6 migrations. The original
request remains immutable across retries; each delivery attempt carries its own
ID, checksum, Linux boot identity and monotonic origin. Duplicate imports cannot
restore grants, erase later checkpoints or extend deadlines. Delivery consumes
lease time. A changed boot or refreshed attempt requires a new relay response.

Files use bounded strict JSON, private owned storage, mandatory SHA-256 pinning,
fsync and exclusive atomic publication. The operator obtains the expected digest
through a trusted channel. This is a trusted-courier design; a digest supplied
alongside an untrusted file does not authenticate it. It adds neither file
encryption nor a new signing authority. Credentials can stay on the courier.

File-only configuration disables direct HTTPS actions and automatic sync children.
During review, status still advertised the inherited HTTPS contact deadline even
though no contact would occur. A separate fix makes `sync_due_in` null and updates
the supervisor and fixtures accordingly. Code comments explain publication,
transaction, replay, timing and network boundaries.

The server checks current authorization and grant ownership/generation/expiry
before exporting cached sync receipts. Bound denial files pause imported work
while retaining unacknowledged results. Revocation cannot stop an already
disconnected executor; recovery requires stopping the old owner and withdrawing
its old journals/files. No local lease extension or automatic expired-work
reallocation is provided.

## Validation

[The acceptance manifest](baselines/C22_VALIDATION.json) records source, publication,
binary and artifact hashes. [Raw CLI reports](baselines/C22_OFFLINE.json) preserve
commands and public-fixture outputs; [the log archive](baselines/C22_LOGS.tar.gz)
contains builds, tests, hardware inventories and checkpoint/fleet reports. No
credential contents, private keys or journals are included.

The hardware fixture stops both Apache and
the coordinator during xpoint/BSGS execution, then restarts them to reconcile.
It uses public scalar-1 fixtures and disposable credentials/journals.

| Check | Result |
| --- | --- |
| Plain CPU release | 34/34 pass, including storage/checkpoint schema-v7 regressions |
| Coordinator CPU release | 51/51 pass; four affected gates pass after the separate status correction |
| Focused debug | 6/6 pass on the final code and clock-regression fixture |
| Focused ASan/UBSan | 5/5 pass with leak detection and halt-on-UB enabled |
| MI300X coordinator/checkpoint/storage | 29/29 pass, including five hardware gates |
| H200 CPU-only subset | 20/20 pass in the native CUDA build |
| H200 disconnected execution and hardware regressions | All five pass; four initially, fleet after its Git metadata setup fix |

Native tests cover in-transit reservation exclusion, ten-block exact union and
four verified matches, foreign/conflicting/duplicate imports, incompatible
manifests, reboot/clock rollback, elapsed delivery time, current revocation,
stale generations and expired cached grants. A 130-checkpoint fixture forces
bounded uploads; newer checkpoints remain queued until their own exchange.
Four process-exit points around export/import COMMIT check restart recovery,
including atomic outbox deletion and acknowledgment. The v6 migration preserves
one sealed v6 snapshot. File tests reject checksum errors, ambiguous fields,
oversized input, symlinks, hardlinks, public permissions and FIFOs.

The live CLI fixture additionally checks wrong authorities/CAs, revoked
credentials, exclusive output preservation, duplicate acknowledgments, retained
executors across two grants and absence of a network child. Xpoint covers
`[1, 0x401)` in two 512-scalar blocks; BSGS covers `[1, 0x10001)` in two
32,768-scalar blocks with `m=257`. Each produces one scalar-1 match. Local block
coverage/results and coordinator results are inspected independently; the outbox
clears only after the worker imports its matching acknowledgment.

## Builds and evidence

The code/test snapshot is `403fd61`; the last production change is `cc9da27`.
Later C22 commits publish guides and acceptance evidence. The manifest checks
source/test and publication hashes, records the binary hashes from each live
report, and hashes every archived artifact. Published v1–v6 SQL and GPU backend
sources are unchanged from C21.

HIP uses `build/hip-release`, the existing gfx942/C19-enabled build with optional
coordinator/HTTPS workers. CUDA uses the isolated H200 build at
`/var/tmp/keyhunt-c22-dc31a34/build`, configured from the `cuda-h200` preset with
those workers enabled. The directory retains its initial snapshot name; its
source was updated to `403fd61` before the final host gates. Native binaries were
built after the status correction. Compiler output and hardware inventories are
in the log archive. Both live offline tests use ordinal 0 with all eight GPUs
visible. These are correctness checks, independent of fleet measurements.

The first CUDA hardware pass completed four gates, including both offline modes.
The fleet gate stopped in its metadata collector because the extracted source
archive lacked `.git`. Fetching the matching history and setting the scratch
checkout to `403fd61` fixes that setup without changing sources or binaries. The
initial failure and focused rerun are both retained in the archive.

The full CPU release run precedes the status-only correction; its four affected
release gates were rerun after that fix. The final debug and sanitizer runs
include the added clock-rollback fixture. HIP's 29-gate run precedes that
additional test case; the two affected native gates pass again afterward. CUDA
includes the new fixture in its focused host checks. No kernel change warrants a
new performance comparison.

## Reproduction and limits

See [the commands and operational workflow](OFFLINE_ASSIGNMENTS.md) and
[the live integration invocation](OFFLINE_ASSIGNMENTS.md#reproduce-the-integration-gate).
Use coordinator-enabled CPU, HIP or CUDA builds. CTest exposes
`coordinator_offline_files`, `coordinator_offline`, `coordinator_offline_failures`
and `coordinator_offline_cli`; the last adds hardware execution for native builds.

No search kernels or launch tuning changed, so C22 adds no performance claim.
The focused hardware regressions cover changed storage, transport and supervision;
prior full arithmetic/search and fleet measurements remain historical evidence.
Live transport checks are localhost mTLS on each host. Physical transfer between
separate hosts, public ingress, thirty-day endurance, MIG/QPX search and genuine
driver-hang recovery remain outside this acceptance. File history remains
retained and consumes disk beyond the bounded outbox. No hosted CI run is claimed.
