# Authenticated coordination (C15)

C15 is in progress. S02 enrollment and project authorization are implemented;
the HTTP/mTLS boundary, machine sync, worker outbox and packaging follow as
separate changes. GPU kernels and checkpoint cadence are unchanged.

The operator selected **isolated localhost validation** for this milestone.
The deployment hostname `dbkeyprogress.rumahsimanis.bagus.my.id` will resolve to
`127.0.0.1` in the test client, with real hostname verification and a dedicated
test CA. System DNS and `/etc/hosts` are not modified. Public ingress and a
physical second host remain explicitly deferred.

## S02: credentials and projects

The optional `KEYHUNT_ENABLE_COORDINATOR` build uses OpenSSL 3 and nlohmann JSON
3.10 or newer. The `coordinator-release`, `coordinator-debug` and
`coordinator-sanitizers` presets are CPU-only and require no GPU SDK.

Schema 3 adds clients, exact certificate fingerprints, SPKI fingerprints,
issuer/serial/validity records, project memberships, job pause controls and an
administrative audit trail. Schemas 1 and 2 remain byte-for-byte unchanged.
An upgrade retains one sealed `pre-v3-*` snapshot of the committed source
version before applying any migrations.

The local administration interface requires explicit first-operator bootstrap.
CA trust alone never grants a role: each certificate must be registered and
active, its client enabled, its validity current, and its project membership
sufficient for the operation. Rotation registers a second exact certificate
against the same client; the old certificate can then be disabled. Identical
common names do not identify the same client.

Readers see project metadata and progress; workers will additionally submit
their own assigned work; project owners create jobs, manage memberships, pause
jobs and inspect results. Progress access does not grant result-data access.
An unauthorized project and an unknown project both return not found.

Each mutation checks current credentials and roles in the same outer SQL
transaction as its writes. Existing repository methods use nested savepoints:
an inner operation cannot commit an incomplete network transaction. Revocation
is checked again on each request, including a persistent TLS connection.
Only the outer COMMIT acknowledges durable changes.

The service accepts canonical job inputs and verifies target/algorithm digests.
It needs the BSGS configuration and table checksum, but does not load a resident
worker BSGS table. Authenticated workers are trusted to report exhaustive
no-match coverage; mTLS is identity, not proof of GPU computation.

## S02 validation

`coordinator_registry` uses real generated EC certificates without writing
private keys into the checkout. It checks explicit bootstrap, unknown/expired/
revoked certificates, strict JSON duplicate rejection, required clientAuth EKU,
same-CN isolation, two projects with identical job manifests, role restrictions,
rotation, membership removal, client disable and transactional rollback.

The coordinator release build passed this gate plus the existing database,
state CLI, checkpoint, checkpoint-failure, control and checkpoint CLI gates:
**7/7**. Existing fourteen checkpoint process-exit cases continue to pass.
The HTTP boundary will independently test CA and TLS authentication.

## S03: private HTTP and real mTLS

The coordinator executable listens only on two Unix sockets. The API socket is
0660 and checks the configured Apache UID with `SO_PEERCRED`; the local admin
socket is 0600 and accepts only the service UID. Their parent directories must
be owned by the service user, without group write or access for other users.
Use a dedicated service account and a shared Apache group in deployment. The
isolated test runs both processes as the current user; it separately tests a
mismatched proxy UID. A process running as the trusted service/Apache account is
inside this boundary and must not host unrelated applications.

`deploy/apache/keyhunt.conf.in` requires client certificates, replaces all
external copies of its four identity headers, and forwards a single-line base64
certificate. The application matches the exact enrolled DER, validates current
registry state on every request, and checks the forwarded Host, SNI and TLS
version. Only `/api/v1/` is proxied. Administration has no HTTPS route. Backend
requests reject duplicate headers, ambiguous framing, duplicate JSON keys,
early data, oversized bodies and unsupported methods. Read/write deadlines,
an 8 MiB body limit and a bounded per-credential request budget limit resource use.

The template uses [Apache header replacement and expressions](https://httpd.apache.org/docs/2.4/mod/mod_headers.html)
and [mod_ssl connection variables](https://httpd.apache.org/docs/2.4/mod/mod_ssl.html).
It was exercised with Apache 2.4.52 and OpenSSL 3.0.2 using patched Ubuntu
packages extracted under `/tmp`; no packages, system services or trust stores
were changed. No GPU libraries are required by `keyhunt-coordinator`.

Configure `KEYHUNT_TEST_APACHE_ROOT=/` for installed Apache, or an extracted
package root containing `usr/sbin/apache2` and `usr/lib/apache2/modules`.
`coordinator_tls` creates private, temporary server/client CAs and runs actual
Apache and coordinator processes. It passed required/unknown/wrong-CA client
checks, forged duplicate header replacement, wrong Host/SNI rejection, API
socket peer rejection, framing/body limits, admin isolation, rotation and
revocation on the **same persistent frontend TLS connection**. Certificates,
keys and databases are removed with the fixture. Public TCP 443 and ACME
renewal remain outside this localhost gate.

## S04a: atomic machine synchronization

`POST /api/v1/sync` accepts protocol 1 with the exact capability list
`["checkpoint-v1", "offline-lease-v1"]`. A request identifies a persistent worker
instance and idempotency key, authorized jobs with device queues, checkpoint
pages, and returned unstarted spares. The authenticated client UUID plus instance
identifies ownership; supplying a different owner in a grant is rejected.

Each device has one active block and optionally one spare. Selection is sequential,
random or random-window, always from unexplored space. Completed, paused and
expired assignments are not normally claimable. The default assignment lifetime
is 2,592,000 seconds; routine sync is 7,200 seconds. Block width remains an explicit
job input to calibrate toward about twelve hours on the reference GPU.

Schema 4 adds machine/device mappings and immutable sync receipts. Every included
project requires a current worker/owner role, including on retries. Progress,
CPU-verified matches, completion, renewals, returns, replacement claims and the
exact response commit in one transaction. Reusing a key with different content
fails. Retrying identical content returns the original response and expiry,
including after a lost completion or renewal acknowledgment. The HTTP envelope
supplies a fresh server time independently of that immutable receipt.

Pages are bounded to 64 checkpoints and 4,096 match observations per request,
128 assignment updates/returns and 64 device queues. Exact target relations are
verified before committing coverage. Authenticated workers remain responsible
for honest exhaustive no-match coverage. Checkpoint receipts retain the existing
C13 audit representation; no second implementation of the coverage union or
allocator was introduced. Jobs and progress remain scoped by project even when
their immutable manifest hashes are identical.

The worker's durable import, bounded outbox and scheduler are the next S04 slice;
this server transaction alone does not yet provide offline worker operation.

S04a validation passed `coordinator_sync` and `coordinator_sync_failures`:
concurrent two-client claims, two mock device queues, identical jobs in separate
projects, reader denial, mixed-project rollback, forged ownership, incompatible
protocol, invalid-match rollback, partial/full coverage, unstarted returns,
twenty-day pause/renewal, thirty-day expiry, and exact lost-response retries.
Four additional child-process exit points cover results, coverage, pre-COMMIT
and post-COMMIT; reopening and retrying preserves one replacement assignment.
The existing database, migration, checkpoint CLI, registry and live TLS gates
also passed after the schema upgrade.

## S04b: durable worker state and offline leases

Schema 5 adds worker settings, imported grants, a checksummed upload outbox and
one immutable pending machine request. Local checkpoint results, coverage,
receipts and upload pages commit together. Pages contain at most 512 matches;
coverage is placed on the last page of each checkpoint. A sync uploads at most
64 pages/4,096 matches, and deletes only the acknowledged snapshot's pages.
Checkpoints committed during HTTPS remain pending for the next sync.

The default outbox budget is 64 MiB (configurable from 1 MiB to 1 GiB). Exhaustion
rolls back the whole checkpoint and stops further execution; it never discards
unacknowledged results or credits lost coverage. Receipt history remains retained
for recovery/audit and consumes additional disk space; the outbox limit is not a
whole-database quota. Disk monitoring and backup retention remain necessary.

Grant import commits before dispatch. Local claim, renew, recover and return
commands cannot mint or extend remote work. Checkpoint authorization checks both
the local assignment fence and a saved Linux boot UUID/CLOCK_BOOTTIME deadline.
A reboot forces revalidation. Fresh authenticated server time and the request's
send time determine the deadline, with a 60-second drain margin; cached receipts
cannot extend it. A wall-clock rollback does not extend monotonic validity.

The persisted machine schedule is 7,200 seconds. Lost replies retain the exact
pending payload. Restart, block completion, matches, an empty queue and remaining
upload backlog do not trigger early contacts. An explicit manual sync can send
or retry a page. Status distinguishes local completion awaiting sync from server
acknowledgment, and reports outbox usage, last acknowledgment, expiry, pause and
revalidation requirements. Simultaneous execution on multiple GPUs remains C20;
these tests exercise distinct device queues with the existing bounded CPU runner.

S04b passed **11/11** focused gates, including two mock GPUs, lost replies,
checkpoints arriving during HTTPS, boot/deadline fencing, outbox exhaustion and
restart scheduling. Four worker process-exit points cover saved snapshot,
received response, pre-acknowledgment COMMIT and post-COMMIT. A hundred-checkpoint
fixture verifies bounded multi-page upload and that results arrive before full
coverage can finish a block. Existing C13/C14 storage/control gates still pass.

## S04c: HTTPS and process supervision

`keyhunt-worker` provides `configure`, `configuration`, `status`, `sync`,
`scheduled-sync`, `next`, `api` and `self-test`. Use `--state-dir DIR` for local
state, `--config FILE` for initial configuration, `--request FILE` for an API
request, and `--device N` for dispatch/self-test. `sync` is explicitly manual;
`scheduled-sync` sends nothing before the persisted due time.

The libcurl transport requires HTTPS, client PEM credentials, a private owned
key file, server CA validation and hostname verification. Redirects, proxy
inheritance, HTTP fallback and insecure modes are disabled. Optional `resolve`
uses `HOST:PORT:127.0.0.1` while retaining the real Host/SNI. Connect/total deadlines
are 10/30 seconds; responses remain bounded to 8 MiB. The coordinator executable
has no libcurl or GPU runtime dependency; the HTTPS worker is a separate target.

`tools/coordinator_worker.py` (installed as `keyhunt-supervise`) owns a stable
supervisor lock and starts separate network and GPU child processes. It runs
fresh HIP self-tests before requesting work, covering hit/miss/boundary/tail
vectors through both xpoint kernels and BSGS groups 1/8. Device visibility and
partition discovery use the existing backend; no partition mode is hardcoded.
It records the observed UUID, partition, CU count, runtime and driver privately.

The supervisor keeps C14's one-executor-per-journal guard. Multiple device queues
are supported and exercised with mocks, but production execution is serial
across those queues until C20. Use one selected device for C15 production work.
Default stepped xpoint and automatic BSGS group selection are retained. BSGS
requires an explicit local `--table`; the service never loads that table.

SIGUSR1/SIGUSR2 pause/resume the GPU child, and SIGINT/SIGTERM request a bounded
drain. Three failed launches or a 300-second progress stall quarantine the device
queue; `--retry-failed` explicitly clears saved failure counts after repair.
No GPU reset is attempted. `--once` consumes saved queues and exits without an
extra final sync. `supervisor.json`, `self-tests.json`, `execution.log` and
`sync.log` live in the private worker state directory.

Native two-worker HTTPS tests passed with separate state/certificates, wrong-CA
and hostname rejection, cross-project denial and persisted no-contact intervals.
The HIP gate additionally passed supervisor-driven xpoint and BSGS execution,
CPU-verified result upload, and local-complete/server-in-progress separation.
Hardware: existing MI300X SPX/NPS1; no partition settings changed. CPX/QPX retain
the same visible-device/CU-aware execution paths, without claiming a new live
CPX/QPX hardware test in this milestone.
