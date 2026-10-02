# C22 offline assignment audit — 2026-10-01

C22 passes this independent audit within its **trusted-courier** contract. No
new P1/P2 defect was reproduced. Fresh CPU, MI300X and H200 checks pass, including
actual file delivery between two hosts, disconnected GPU execution, exact
coverage, retained results and acknowledgment. This is a correctness and recovery
audit; it adds no GPU throughput claim.

The audited revision is `7b12569e9f1b53099e42ed2e48a552c088e62d31`.
Local and remote builds use separate detached checkouts of that exact revision.
Concurrent C23 changes were excluded. Production code was not edited.
The separate A21 follow-up below uses `85790f0`; its result does not retroactively
change the C22 snapshot.

[Validation manifest](C22_AUDIT_VALIDATION.json),
[independently checked evidence](C22_AUDIT_EVIDENCE.json),
[cross-host report](C22_AUDIT_CROSS_HOST.json) and
[raw logs and probes](C22_AUDIT_RAW_LOGS.tar.gz) retain the checks and reproduction
details. The implementation's acceptance record remains [C22_VALIDATION](../C22_VALIDATION.md).

## Results

| Fresh check | Result |
| --- | --- |
| CPU Release, coordinator and HTTPS enabled | 51/51 pass; 32.40 s |
| MI300X HIP, affected coordinator/storage/checkpoint tests | 28/28 pass; 59.93 s |
| H200 CUDA, affected coordinator/storage/checkpoint tests | 29/29 pass; 155.05 s |
| Focused host ASan/UBSan, leak detection and halt-on-error | 5/5 pass; 9.44 s |
| Additional native transaction/deadline/authorization probes | Five scenarios pass |
| Concurrent exclusive file publication | One winner among eight writers; eight later copies identical |
| Populated v6 worker and coordinator migration | Existing table contents preserved; pending request and results retained |
| Cross-host courier, H200 ordinal 7 | Both modes pass; four blocks, two expected matches |

The HIP and CUDA selections include native offline file/reconciliation/failure
tests, localhost mTLS offline execution, ordinary HTTPS workers, checkpoint
execution and pause/control regressions, and a two-GPU fleet smoke test. CUDA
also passes its selected-context isolation test. These are focused selections,
not fresh full arithmetic/search suites. The complete CPU suite runs the host
regressions. No test in these selections was skipped.

HIP uses `gfx942`, with `KEYHUNT_GFX942_CARRY=OFF`. CUDA targets `sm_90` with its
normal optimized backend. Both hosts expose eight GPUs. Fresh localhost offline
tests select ordinal 0; the cross-host test selects H200 ordinal 7. Source,
binary hashes, configuration, commands, hardware inventory and JUnit results are
retained. No clocks, power settings or partitions were changed.

## Source and boundary review

The server's [offline-sync path](../../src/coordinator/repository.cpp) reuses the
machine-sync transaction, then checks current assignment ownership, generation,
epoch and expiry before returning a cached receipt. Current authentication and
project permissions are checked before receipt lookup. Current pause controls
are returned separately from cached allocation data.

[Export/import](../../src/coordinator/worker.cpp) persists the delivery attempt
before publication and binds a response to that attempt and request digest.
Nested journal operations use savepoints inside the outer import transaction:
grants, outbox deletion, acknowledgments and the transfer receipt commit together.
Accepted or denied duplicate responses cannot restore old grants or advance
deadlines. A refreshed attempt retains the immutable machine payload while
invalidating the previous attempt. Delivery consumes the saved lease budget.

[File publication and reading](../../src/coordinator/offline.cpp) enforce the
8 MiB bound, private owned files/directories, expected SHA-256, strict JSON and
exclusive publication. The implementation syncs the temporary file before
`renameat2(RENAME_NOREPLACE)` and then syncs the directory. The fresh concurrency
probe exercises separate processes racing on the same output name; exactly one
succeeds, the others preserve its bytes, and no staging file remains after these
ordinary failure paths.

Digest pinning authenticates the operator's selected bytes only when the expected
digest arrives through the trusted channel required by the design. This audit
does not reinterpret it as a signature scheme. A disconnected worker cannot
learn a later revocation until another exchange; stopped-owner recovery remains
necessary, as the operator guide states.

## Additional adversarial checks

The retained native probe links the unchanged production libraries and reuses
only the existing fixture setup. It adds these checks:

- An invalid fourth grant rolls back the earlier three imports, leaves the same
  pending exchange, and permits the correct response to be imported afterward.
- Replaying a cached server receipt after pausing its job returns the current
  pause control. The worker stays paused until a later authorized exchange.
- A denial with real queued results pauses runnable work without deleting the
  results. A new transfer retains the identical pending machine request and
  subsequently reconciles its checkpoint page.
- A membership downgrade to `reader` prevents retrieval of a cached receipt.
- A response delivered after the entire lease can record its receipt but leaves
  neither device with executable work. Its duplicate remains a no-op.

The existing fresh native gates additionally exercise reservation exclusion,
ten-block exact union, foreign/conflicting imports, canonical manifests, clock
rollback, changed boot identity, superseded attempts, 130-checkpoint paging,
new checkpoints arriving in transit, revocation, generation fences and four
process-exit points around durable export/import.

The migration probe creates **actual schema-v6 journals using the frozen C21
libraries**, with completed local coverage, pending upload pages, a saved
unacknowledged machine request and active coordinator reservations. C22 upgrades
both journals to v7. Every pre-existing table except the migration ledger has
the same row count and content digest afterward. HTTPS configuration and worker
status survive. Each sealed backup retains schema v6 and the same tables, with
only the intentional epoch rotation and quarantine metadata changes. This extends
the implementation's reconstructed empty-v6 migration fixture.

Two initial probe assumptions were corrected: the role name is `reader`, and a
sealed backup intentionally changes epoch/quarantine metadata. Neither was a
production failure; the raw harness notes record both corrections.

## Actual file delivery between hosts

The local MI300X host (`xe9680-waco4`) runs the CPU coordinator and courier.
The remote H200 host (`xe9680l-h200`) runs the CUDA worker. Their boot identities
differ. Only public configuration and request/response files cross hosts;
fixture credentials remain with the courier. Ten SCP transfers are checked
against matching SHA-256 values at both endpoints.

| Mode | Exact range | Blocks | Expected result |
| --- | --- | --- | --- |
| Xpoint | `[1, 1027)` | Two × 513 scalars | Scalar 1 |
| BSGS, `m=257`, one target | `[1, 65539)` | Two × 32,769 scalars | Scalar 1 |

The test stops both Apache and the coordinator during execution. The remote
worker has file-only configuration without CA/certificate/key paths; direct
sync is rejected and no sync log is created. Each mode retains one executor
across its two grants, with one cold and one warm grant. Local journal inspection
confirms each exact interval, no remaining coverage, and the expected public
generator match. The coordinator reports no completed blocks before reconciliation.
After services restart, the exported result page is relayed and its acknowledgment
imported. Both server blocks finish, results agree, the outbox clears, and a
duplicate acknowledgment changes nothing. Journal integrity checks pass.

SSH provides the trusted transfer and process-control channel for this test.
This closes the earlier untested **cross-host file-delivery** case for these
fixtures; it is not a physical air-gap, removable-media or public-ingress test.
HTTPS remains confined to the courier host's localhost fixture.

## GPU performance implications

All 45 tracked files selected under the kernel, backend, scheduler and device
worker paths are unchanged from C21. Persistent executors still survive both
offline grants, so this transport does not introduce the old per-grant context
and BSGS-table setup cost. Tiny correctness fixtures are unsuitable for a new
MI300X/H200 throughput comparison. The [C21 measurements](C21_AUDIT.md) and A22's
measured BSGS batch-sizing opportunity remain the relevant performance evidence.

For offline operation, **size reserved work for the courier interval**. The
existing allocator supplies one active block and at most one spare per device.
The calibration tool predicts twelve hours per block on its measured reference
GPU: two such blocks predict roughly one day of work, not thirty days of busy
execution. Faster GPUs can exhaust their queues sooner. This is an inference
from the queue cap and calibration policy, not a new endurance measurement.

Before creating an offline job, use measurements for its actual targets/table
and hardware, account for delivery delay and variability, and choose a fixed
block width that supplies enough work until the next exchange within the lease.
The bounded local work units and checkpoint cadence remain independent of that
width. A courier-aware inventory recommendation is a useful follow-up to A08;
this audit does not change queue limits or tune kernels without measurements.

## A21 follow-up at a separate revision

Commit `85790f0d2a3349245d8c8b4e5d5dfeb8ee84011e` adds an independent terminal
deadline that survives subsequent log events and resets for a new child. A fresh
old/new probe reproduces the missing deadline at C22 and observes it at the
fixed revision. The fixed supervisor fixture passes in 12.78 s, including real
subprocesses, a terminal child that ignores SIGTERM, bounded retries/quarantine,
a healthy peer and file-only operation. Its injected supervisor clock accelerates
the long deadlines; it does not induce a GPU driver hang.

**A21 is closed for this tested supervisor path at `85790f0`.** The evidence is
in [the follow-up report](C22_AUDIT_TERMINAL_FOLLOWUP.json). A22 remains a measured
tuning opportunity outside C22's transport scope.

## Evidence and reproduction

The independent [evidence checker](../../tools/audit_c22.py) verifies all 18
published source/test fingerprints against the named acceptance revision, 12
publication fingerprints, six unchanged migration files, two artifact hashes
and all 41 archived log-member hashes. It reconciles the recorded binary hashes
with the retained CLI reports and independently recomputes exact coverage,
public results, device steps and executor reuse for retained and fresh reports.
Matching historical hashes establish consistency, not an independent rerun of
those historical builds. Fresh builds and runs supply that additional evidence.

```sh
python3 tools/audit_c22.py --source /path/to/frozen-c22 \
  --report docs/audits/C22_AUDIT_HIP_OFFLINE.json \
  --report docs/audits/C22_AUDIT_CUDA_OFFLINE.json \
  --report docs/audits/C22_AUDIT_CROSS_HOST.json \
  --output /tmp/c22-evidence.json

python3 tools/audit_c22_cross_host.py --source /path/to/frozen-c22 \
  --build /path/to/cpu-coordinator-build --apache-root /path/to/apache-root \
  --host bagus@100.83.167.144 --remote-source /path/to/remote-frozen-c22 \
  --remote-build /path/to/cuda-build --device 7 --output /tmp/c22-cross-host.json
```

The raw archive retains build/test commands, native probe sources, compilation
commands, migration harness, configuration and reports. It excludes credentials,
private keys, journals and binaries. Long-duration disconnected execution,
power-loss durability, genuine driver hangs, public deployment and encrypted or
signed file formats were not tested. Retained transfer history still consumes
disk beyond the bounded outbox; C22 documents that limit.
