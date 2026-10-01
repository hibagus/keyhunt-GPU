# Authenticated coordination (C15)

C15 implements authenticated coordination, durable machine sync, a worker
outbox, process supervision and recovery. The accepted deployment scope is
isolated localhost. GPU kernels and checkpoint cadence are unchanged.

The operator selected **isolated localhost validation** for this milestone.
The deployment hostname `dbkeyprogress.rumahsimanis.bagus.my.id` resolves to
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

Readers see project metadata and progress; workers additionally submit
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

The local admin operation names and required fields are:

| Operation | Fields in addition to `operation` |
| --- | --- |
| `bootstrap`, `client-add` | `name`, `certificate` (public PEM leaf) |
| `credential-add` | `client`, `certificate` |
| `credential-set` | `fingerprint`, `enabled` |
| `client-set` | `client`, `enabled` |
| `project-create` | `name`, `owner` (registered client UUID) |
| `membership-set` | `project`, `client`, `role` (`reader`, `worker`, `owner`, `none`) |
| `clients`, `check` | None |
| `backup` | `destination` (new external directory) |
| `activate-restore` | Three required stopped/review assertions, described under S05 |

Private keys are never enrollment fields. Worker configuration contains file
paths to credentials, not private-key content. The service does not expose
administrative enrollment over HTTPS.

## S02 validation

`coordinator_registry` uses real generated EC certificates without writing
private keys into the checkout. It checks explicit bootstrap, unknown/expired/
revoked certificates, strict JSON duplicate rejection, required clientAuth EKU,
same-CN isolation, two projects with identical job manifests, role restrictions,
rotation, membership removal, client disable and transactional rollback.

The coordinator release build passed this gate plus the existing database,
state CLI, checkpoint, checkpoint-failure, control and checkpoint CLI gates:
**7/7**. Existing fourteen checkpoint process-exit cases continue to pass.
The S03 HTTP boundary independently tests CA and TLS authentication.

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

`POST /api/v1/sync` accepts protocol 1. Current workers send the exact capability
list `["checkpoint-v1", "offline-lease-v1", "hash160-v1", "ethereum-v1", "vanity-v1", "minikeys-v1", "scalar-stride-v1", "scalar-reverse-v1", "scalar-orbit-v1"]`.
Updated coordinators also accept the older two-element list for xpoint/BSGS and
the three-element list for those modes plus HASH160, and the four-element list
that also supports Ethereum, the five-element list adding vanity, the six-element
list adding minikeys, the seven-element list adding forward scalar strides,
and the eight-element list adding reverse traversal.
HASH160, Ethereum, vanity, minikey, strided, reverse and orbit jobs
require their respective capability before reservation, renewal, update or receipt replay;
incompatible requests receive HTTP 426. Deploy the updated coordinator before
updated workers. Unknown capabilities and wire modes fail explicitly. A request
identifies a persistent worker instance and idempotency key, authorized jobs with device queues, checkpoint
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

The S04b worker slice below supplies durable import, a bounded outbox and the
persisted scheduler needed for offline operation.

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

The persisted HTTPS machine schedule is 7,200 seconds. Lost replies retain the exact
pending payload. Restart, block completion, matches, an empty queue and remaining
upload backlog do not trigger early contacts. An explicit manual sync can send
or retry a page. Status distinguishes local completion awaiting sync from server
acknowledgment, and reports outbox usage, last acknowledgment, expiry, pause and
revalidation requirements. [C20 multi-GPU execution](MULTI_GPU.md) now consumes
these machine queues concurrently through separate persistent device owners.
[C22 file transport](OFFLINE_ASSIGNMENTS.md) reuses these snapshots and deadlines
with manual export/relay/import, schema-v7 transfer receipts and no network child
on the disconnected worker.

S04b passed **11/11** focused gates, including two mock GPUs, lost replies,
checkpoints arriving during HTTPS, boot/deadline fencing, outbox exhaustion and
restart scheduling. Four worker process-exit points cover saved snapshot,
received response, pre-acknowledgment COMMIT and post-COMMIT. A hundred-checkpoint
fixture verifies bounded multi-page upload and that results arrive before full
coverage can finish a block. Existing C13/C14 storage/control gates still pass.

## S04c: HTTPS and process supervision

`keyhunt-worker` provides `configure`, `configuration`, `status`, `sync`,
`scheduled-sync`, `next`, `api`, `self-test` and `run-device`. C22 adds
[`file-export`, `file-relay` and `file-import`](OFFLINE_ASSIGNMENTS.md#user-commands). Use `--state-dir DIR` for local
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
supervisor lock, one separate network child and a persistent native process per
selected device. Each device process runs fresh xpoint/BSGS/HASH160/Ethereum/vanity/minikey self-tests before
execution. Device selection queries only its ordinal, records the observed UUID,
partition, CU count and runtime/driver versions, and never changes partition modes.

Each process transactionally claims a distinct block. Faster devices may take
unstarted queued grants for the same job; active blocks remain owned until their
process stops. Targets and GPU tables stay loaded across block handoffs. The
standalone whole-journal guard is retained; supervised execution uses per-block
guards plus per-device process locks. Default stepped xpoint and automatic BSGS
group selection remain. HASH160, Ethereum and vanity use the scalar batch/kernel options and retain
their immutable targets across grants. BSGS requires a matching local `--table`.

The job API accepts mode `hash160`, configuration `khsearch`, version 1, mode 3,
zero `m` and zero table checksum. `targets` is canonical sorted unique 21-byte
relations encoded as lowercase hex: tag `01`/`02` plus 20 hash bytes. It carries
no address-text parsing or separate encoding flag; the tags are authoritative.
Existing bounded wire-size limits still apply. The [checkpoint CLI](CHECKPOINTS.md#bitcoin-address-and-hash160-jobs)
accepts raw/address files and computes this canonical binding.

Ethereum wire jobs use mode `ethereum`, version-1 configuration with mode byte 4,
zero `m` and checksum, and sorted unique **20-byte** binary addresses encoded as
hex. ERC-55 is an input-file validation rule, not a wire capitalization rule.
Updated device owners retain targets across grants and test both Ethereum kernel
variants on their selected ordinal. [Ethereum contract](C23_ETHEREUM.md).

Vanity wire jobs use mode `vanity`, configuration version 1 with mode byte 5,
zero `m` and checksum, and sorted unique **36-byte** targets encoded as hex.
Each target contains tag 1/2, prefix length, case-sensitive ASCII prefix and zero
padding. No text-case normalization or prefix merging is allowed. All overlapping
prefix/encoding relations survive local commits, upload and acknowledgment replay.
Owners test both kernels before execution and retain the executor across grants.
See [vanity contracts](C23_VANITY.md).

`--devices 0,1` selects configured queues. `--device-map QUEUE=ORDINAL` handles
visibility renumbering while retaining the UUID binding. A different physical or
logical UUID requires `--rebind-device QUEUE` and a stopped old owner. Omitting a
queue preserves its active block. `--host-memory` caps each device's table/target
budget; `--host-memory-total` also bounds their sum. Runtime/SQLite overhead is
additional. See [ownership, memory and calibration details](MULTI_GPU.md).

SIGUSR1/SIGUSR2 pause/resume all selected owners; SIGINT/SIGTERM drain them. For
one queue, use `keyhunt checkpoint pause|resume|stop|status --state-dir DIR
--slot QUEUE`. A confirmed socket pause is excluded from the watchdog. Expected
coordinator pause, expiry and fencing events do not count as GPU failures.
Three execution failures or a 300-second progress stall quarantine that queue;
`--retry-failed` clears saved counts after repair. Drain and kill waits are bounded
across the fleet; no GPU reset is attempted and surviving owners retain OS locks.

`--once` consumes executable saved queues without an extra final sync.
`supervisor.json`, `self-tests.json`, `execution-QUEUE.log` and `sync.log` stay in
the private worker directory. Device logs retain one rotated 8 MiB predecessor.
Completed batches, not diagnostic output growth, drive progress detection.

Native two-worker HTTPS tests passed with separate state/certificates, wrong-CA
and hostname rejection, cross-project denial and persisted no-contact intervals.
The HIP gate additionally passed supervisor-driven xpoint and BSGS execution,
CPU-verified result upload, and local-complete/server-in-progress separation.
Hardware: existing MI300X SPX/NPS1; no partition settings changed. CPX/QPX retain
the same visible-device/CU-aware execution paths, without claiming a new live
CPX/QPX hardware test in this milestone.

## S05: recovery and coordinator-only installation

An owner can transfer an in-progress block with
`POST /api/v1/projects/P/jobs/J/blocks/B/recover`. Its JSON fields are `client`,
`instance`, `device`, `request` and `previous_executor_stopped: true`. The target
client must have a current worker/owner membership. Even an expired transfer
requires the stopped-executor assertion. The operation fences the old generation,
retains accepted partial coverage/results and is replay-safe. Use a fresh worker
journal/instance as the destination; its first sync imports accepted coverage and
resumes the exact complement. Earlier accepted results remain on the server.
Keep the old journal for unacknowledged data and audit; this release does not
silently rewrite a live journal's generation or pending request.

Owner-only results are paged with `GET .../results/AFTER/LIMIT` (1–1,000 rows);
`GET .../results` remains the first 100 rows. Every route checks current project
membership. Bounded per-credential, per-client and client/project budgets apply.
An explicit online authorization/fencing refusal pauses local dispatch while
retaining its pending request/outbox. Transport outages retain already-valid
offline authority. Current pause controls travel outside the immutable sync
receipt, so retrying an old response cannot bypass a newer pause.

Local admin requests are JSON files, for example:

```sh
keyhunt-coordinator admin --socket /run/keyhunt-coordinator/admin.sock --request /private/request.json
```

Use `{"operation":"backup","destination":"/private/new-snapshot"}` for a
consistent sealed snapshot. `keyhunt-coordinator restore --source SNAPSHOT
--state-dir RESTORED` restores it into quarantine (see [STORAGE.md](STORAGE.md)). All remote reads/writes remain disabled while
quarantined. The local `check` operation remains available.

Activation deliberately supports only the conservative stopped-executor route:

```json
{"operation":"activate-restore","old_authority_stopped":true,"all_previous_executors_stopped":true,"access_review_complete":true}
```

These are operator assertions requiring actual reconciliation, not a way to make
offline executors stop. If any old executor/authority might still run, leave the
restore quarantined. No automatic timeout or duplicate-computation override is
implemented. Activation disables **every** restored client and certificate;
review memberships and explicitly enable clients/enroll fresh credentials before
use. This prevents a backup from resurrecting later-revoked access. The new epoch
fences old writes, but only stopped executors make old free space safe to reuse.

Build/install only the CPU service:

```sh
cmake --preset coordinator-server
cmake --build --preset coordinator-server -j12
cmake --install build/coordinator-server --prefix /desired/prefix --component coordinator
```

This preset disables the HTTPS worker and needs no HIP/CUDA SDK or libcurl.
Dependencies are a C++ compiler, SQLite >=3.51.3, OpenSSL 3 and nlohmann JSON >=3.10.
The install component includes the binary, Apache/systemd templates and notices;
it does not enable services or alter the host. Render the templates with a
dedicated service UID, Apache UID/group, paths and authority. Socket parents are
0750 service-owned/shared-group; API is 0660 and admin is 0600. Keep the database
on local storage with free space for WAL and retained history. Configure writable
backup destinations inside the service's allowed state storage, or use a reviewed
systemd `ReadWritePaths` override for another local backup directory.

An isolated install at `/tmp/keyhunt-c15-package` passed authenticated startup.
This host supplies its CPU SQLite library from ROCm's sysdeps directory; its path
was explicitly set as the test install RPATH. `ldd` shows no HIP/HSA/GPU runtime.
A different host must supply its own compatible SQLite library/runtime search
path; the temporary install is validation, not a portable binary distribution.

S05 passed eight coordinator gates, including actual `SQLITE_FULL` during a
fragmented checkpoint, old-backup quarantine, revoked-access reconciliation,
expired partial transfer, stale-generation rejection and resumption without
recomputing the accepted prefix. Dedicated test binaries contain failure hooks;
the production executables do not.

## S06: isolated localhost operation

The user replaced the physical-second-host/public-ingress gate with isolated
localhost validation. Apache and the coordinator run as separate processes;
two workers have separate journals and enrolled credentials. All listeners bind
to loopback, with Unix sockets between Apache and the database service. This
is process and state isolation, not a VM/container or a boundary against another
process running as the same trusted Unix user.

Build the chosen GPU preset with `KEYHUNT_ENABLE_COORDINATOR=ON` as described
in [BUILD.md](BUILD.md#optional-tuning-and-workers). This supplies both the CPU
service and native worker. From the repository, with Apache installed (or
`--apache-root` pointing to an extracted package root), start the environment:

```sh
build_dir="$PWD/build/hip-release"  # NVIDIA: "$PWD/build/cuda-h200"
demo_dir="$HOME/.local/state/keyhunt-coordinator-demo"
python3 tools/coordinator_local.py \
  --directory "$demo_dir" \
  --coordinator "$build_dir/keyhunt-coordinator" \
  --worker "$build_dir/keyhunt-worker" --port 8443
```

The launcher prints readiness after a real authenticated HTTPS read. It creates
a tiny synthetic xpoint job, two approved test identities, and worker JSON
configurations pointing at
`https://dbkeyprogress.rumahsimanis.bagus.my.id:8443`. Each configuration uses a
client-local `resolve` override to `127.0.0.1`; hostname and CA verification remain
on. Existing identities/state are preserved across restarts. `--check` performs
startup/readiness and stops. Ctrl-C stops both services, retaining private state.

On this machine, append `--apache-root /tmp/keyhunt-c15-deps/root` to use the
already-extracted Apache packages. These temporary dependencies and the validated
`/tmp/keyhunt-c15-local-demo` environment are disposable; they are not system
installations. Install normal dependencies or retain a private package prefix for
long-lived use. Test CA private keys and sixty-day test certificates stay outside
Git and are intended for this isolated environment only.

In a second terminal, while the launcher is running:

```sh
# Set these again: shell variables are not shared between terminals.
backend=hip  # NVIDIA: cuda
build_dir="$PWD/build/hip-release"  # NVIDIA: "$PWD/build/cuda-h200"
demo_dir="$HOME/.local/state/keyhunt-coordinator-demo"
"$build_dir/keyhunt-worker" sync --state-dir "$demo_dir/alice-worker"
python3 tools/coordinator_worker.py \
  --state-dir "$demo_dir/alice-worker" --backend "$backend" \
  --worker "$build_dir/keyhunt-worker" --keyhunt "$build_dir/keyhunt" --once
"$build_dir/keyhunt-worker" status --state-dir "$demo_dir/alice-worker"
```

After execution, status shows local completion awaiting sync. A subsequent manual
`sync` uploads the durable outbox; the supervisor otherwise waits for its regular
two-hour contact. The `bob-worker` configuration provides the second independent
client. Each demo worker has queue `0` only; it does not automatically configure
eight queues. Use [fleet configuration and controls](OPERATIONS.md#concurrent-workers)
for existing multi-queue jobs. This demo's small blocks are for validation, not
twelve-hour production calibration. Public DNS, TCP 443 ingress, ACME issuance/renewal and a physical
second host remain explicitly untested/deferred.

The localhost gate also renews the server leaf under the test CA, gracefully
reloads Apache and verifies the new leaf on a fresh hostname-checked connection.
This validates local certificate replacement/reload; it is not an ACME or public
network renewal test. The launcher gate checks identity-preserving restart,
private key/configuration permissions and rejection of a checkout state path.

A native repository burst with 32 authenticated machines/64 device queues reserved
128 distinct blocks and committed 4,096 fragmented intervals. Median/p95 claim
latency was 1.98/2.18 ms; progress plus renewal was 2.88/3.19 ms after removing
unnecessary curve initialization from no-match updates. Database/WAL sizes were
1,896,448/4,260,112 bytes at capture. These are local CPU/storage samples,
excluding HTTPS, WAN latency and GPU computation. Rate-limit rejection and exact
receipt retries were checked in the same fixture. See the final
[validation record](COORDINATOR_VALIDATION.md) for scope and regression results.

## C23 minikey ordinal jobs

Wire mode `minikeys` uses configuration mode byte 6 and sorted 22-byte targets:
length (22/30), encoding tag (1/2) and HASH160. The coordinator checks the
ordinal domain before persisting a job. Workers must advertise `minikeys-v1`
before allocation, renewal, update or cached receipt replay; deploy the updated
coordinator first. Prior capability sets remain valid for their supported modes.

Checkpoint receipt coordinates retain ordinals even in historical fields named
`scalar`. Public GET results expose `ordinal`, `minikey`, derived `scalar` and
`coordinate_space:"minikey-ordinal-v1"`. Device grant summaries report
`computed_ordinals`. The owner defaults to the direct kernel for this family;
an explicit stepped override fails. The supervisor forwards a kernel override
only when supplied, preserving previous scalar-mode defaults. Targets and GPU
allocations survive grant handoff; startup tests cover both lengths and encodings
on the selected ordinal. See [acceptance](C23_MINIKEYS_VALIDATION.md).

## C23 scalar stride jobs

Version-2 search configuration binds exact positive strides for wire modes
`xpoint`, `hash160`, `ethereum` and `vanity`. It is 146 bytes: the existing header
with version byte 2 and zero table fields, followed by 32-byte big-endian scalar
begin, exclusive scalar end and stride. Job `begin`/`end_exclusive` must equal the
one-based candidate domain `[1,N+1)`; block width counts candidates. API wide
integers retain their canonical `0x` plus 64-hex-digit representation.

Upgrade coordinator and workers together. Strided jobs require the seventh
capability `scalar-stride-v1` before allocation, renewal, checkpoint updates or
cached replies. Older two-through-six-capability workers retain access to their
supported version-1 jobs. Unknown/malformed mappings fail shared binding
validation before import. No schema migration is needed.

Worker startup checks direct, stepped and GLV kernels for every scalar family on its owned
device. The prepared executor retains the immutable SG cache across grants;
`grant-finish` reports `computed_candidates` with the coordinate-space label.
Owner-only result views show candidate index and actual scalar separately. See
[stride contracts](C23_STRIDES.md). Explicit candidate block widths are supported;
existing xpoint/BSGS calibration does not calibrate strided jobs.

## C23 reverse scalar jobs

Version-3 scalar configurations use the same 146 bytes as version 2; version 3
implies reverse order and allows stride one. Job bounds are candidate indices
`[1,N+1)`; the configuration binds original A, B and S. Results expose both
`candidate_index` and actual `scalar`, labelled `scalar-reverse-index-v1`.
Changing order changes job identity; a forward receipt cannot certify reverse
coverage. Receipt bytes and protocol/schema versions remain unchanged.

The eighth capability `scalar-reverse-v1` is required before allocation, renewal,
updates or cached replies for reverse jobs. Previous two-through-seven capability
sets retain their supported forward jobs. Fresh owned-device self-tests exercise
all three kernels and both directions. Prepared executors retain signed point steps
across grants and reject direction mismatches. HTTPS and courier-file transport
share this binding; see [reverse acceptance](C23_REVERSE_VALIDATION.md).


## C23 GLV execution choice

`tools/coordinator_worker.py --kernel glv` forwards the opt-in scalar kernel to
the owned device process. Fresh owners self-test all three scalar kernels before
executing grants. Prepared targets survive grant handoff; GLV needs no stepped
point-power cache. HTTPS and file workers share this execution path.

GLV changes no configuration, schema, receipt or capability list: versions 1/2/3
still describe the same candidate mappings. Upgrade the executing binary to one
that understands `glv`; use the existing stride/reverse capability requirements
for those jobs. The default remains `stepped`. Explicit kernel overrides are
rejected for BSGS; minikeys accepts only `direct`. See [GLV contracts](C23_GLV.md).

## C23 related-key orbit jobs

Configuration versions 4/5 select six-member orbit expansion in forward/reverse
seed order. Original seed A:B and S remain in the 146-byte configuration; job
root `[1,6*N+1)` must agree exactly. Results report expanded index, seed, variant
and derived scalar under `scalar-orbit-index-v1`. Overlapping seeds preserve
separate observations. HTTPS and file transports carry the same immutable bytes.

The ninth capability `scalar-orbit-v1` is required before allocation, renewal,
updates or cached replies for these jobs. Previously supported capability lists
remain valid for their original jobs. Upgrade the coordinator before enrolling
new workers. Workers self-test the mapping on their owned ordinal, retain all
six stepped caches across grants, and stop batches at variant boundaries.
See [contracts](C23_ORBITS.md) and [acceptance](C23_ORBITS_VALIDATION.md).
