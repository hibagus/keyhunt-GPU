# C15 validation and findings

C15 is complete for the user-approved isolated localhost scope. The
[raw evidence](baselines/C15_VALIDATION.json) records commits, source/schema/binary
hashes, raw test output, dependency versions, the local device self-test and
coordinator transaction samples. [Operations and limits](COORDINATOR.md) describe
the implemented commands and deployment boundary.

| Build | Distinct passing gates | Evidence scope |
| --- | --- | --- |
| Coordinator CPU release | 41/41 | Full 39-test run, two added gates, affected-case reruns after fixes/optimization |
| Coordinator CPU debug | 41/41 | Full 39-test run, two added gates, final ten coordinator gates |
| CPU ASAN/UBSAN | 39/39 | Final focused suite with leak detection and halt-on-error enabled |
| HIP release, gfx942 | 57/57 | Full 55-test run, two added gates, final ten coordinator gates |

The sanitizer selector excludes only the existing legacy `cpu_baseline` and
`target_loading` cases. No new failures are waived. The initial fixture leak is
recorded below, separately from the passing final run. Python compilation,
Markdown links/anchors/fences and whitespace checks also pass.

The suite covers enrollment/rotation/revocation, all implemented route scopes,
real Apache mTLS and forged headers, Host/SNI and CA rejection, revocation on an
existing TLS connection, server certificate renewal/reload, concurrent claims,
exact retries, partial/full coverage and CPU result verification. Worker cases
exercise two mock device queues, real supervised HIP xpoint/BSGS, no premature
server completion, lost replies, bounded upload pages, twenty-day pause/renewal,
thirty-day expiry, boot/deadline fencing and online authorization refusal.

Four server and four worker process-exit points supplement C13's fourteen
checkpoint faults. Recovery includes a real SQLite page-limit exhaustion,
old-backup quarantine, credential reconciliation and transfer of a partially
completed block without recomputing its accepted prefix. The CPU-only installed
service also passed mTLS, backup, restore and overwrite-rejection checks.

Validation uses the existing MI300X SPX/NPS1 configuration. No GPU kernel or
partition setting changed. CPX/QPX/SPX discovery contracts remain covered; a new
live CPX/QPX run was not performed. Public ingress, a physical second host and
ACME remain deferred. The local setup uses separate processes/private state under
one trusted Unix user; simultaneous multi-GPU execution remains C20.

## Sanitizer recovery fixture

The first focused ASAN/UBSAN run passed 16 of 17 gates. LeakSanitizer traced the
failure to the recovery test's nested JSON initializer-list wrappers when its
embedded repository request threw for an expired/fenced assignment. The disk-full
transaction assertions themselves passed; no SQLite resource leak was reported.

The fixture now completes the throwing request before constructing a successful
transport envelope. This matches the real HTTPS boundary, which returns an error
without constructing a successful response. No production error handling was
suppressed and no sanitizer exclusion was added. The recovery gate then passed,
along with the new local-launch/budget gates and twenty additional core gates.

## Coordinator transaction cost

The 32-machine burst initially measured 130.08 ms median / 130.43 ms p95 for
progress plus renewal. Inspection found a new secp256k1 verifier constructed for
each updated grant, including lease-only and no-match updates. This unnecessarily
held the write transaction while initializing curve state.

The implementation now resolves a target binding once per job only when a page
contains matches, and creates one verifier per machine sync only when needed.
Every submitted match still receives the same CPU target/scalar verification.
After this change, the identical 32-machine/64-device/128-grant fixture measured
2.88 ms median / 3.19 ms p95 (5.00 ms maximum). Claim latency stayed near 2 ms.
These are local CPU repository timings, not GPU throughput or WAN capacity.
Random block selection changes touched database pages, so database/WAL sizes are
reported as observations rather than attributed to this optimization.
