# C22 offline assignment files

Status: complete within the [recorded acceptance scope](C22_VALIDATION.md). The existing authenticated sync protocol is
the authority for reservations, verified result reconciliation and fenced leases.
C22 adds a manual file transport for a disconnected GPU worker and a connected
courier. It does not add a second allocator or permit standalone grants to be
uploaded as coordinator work.

## Design decisions

A worker first exports a persisted request. A connected courier submits it over
mTLS using that worker's enrolled credential. The coordinator reserves the blocks
in its normal transaction before returning the assignment response file. The
offline worker imports that response before executing. Later request files carry
the same durable checkpoint outbox pages as HTTPS sync. Responses acknowledge only
the exported page snapshot; newer local results remain queued.

A request file by itself does not reserve work. Once the server commits the
response, its blocks are in progress even if a file is lost. Retry the saved
request; never issue a replacement allocation to guess whether delivery succeeded.
The immutable machine request already provides idempotent server reconciliation.
Each file exchange additionally has a persisted transfer ID, request checksum,
Linux boot identity and monotonic export time. Response import binds to these
values. Delivery time consumes the lease: importing or copying a file cannot
restart a thirty-day clock. A changed boot requires a new exchange. Superseded
responses cannot reactivate work; exact duplicate imports are no-ops.

Files are versioned JSON bounded to 8 MiB. Publication writes a private temporary file,
syncs it, atomically publishes without replacing an existing destination, and
syncs the parent directory. Export/import paths remain outside Git checkouts.
Transfers contain public targets and found scalars, so use private storage and a
trusted transfer channel. They never contain client private keys.

The connected courier authenticates the coordinator over HTTPS. File readers
require an expected SHA-256 digest delivered by the operator through a trusted
channel; a checksum shipped with an untrusted file is not authentication. This
keeps authority-key distribution and rotation out of the file format and preserves
the existing mTLS enrollment boundary. The courier is trusted with the worker's
credential and must not relay arbitrary unverified input on its behalf.

File-only worker configuration is immutable and disables automatic network sync.
It retains the same device owners, local controls, deadline checks and bounded
outbox. The default lease is still thirty days; no longer lease or automatic
expired-work override is introduced. Definite authorization/fencing denials can
be carried back as bound response files, pausing execution while preserving data.

Revocation fences reports but cannot interrupt a disconnected GPU. Recovery still
requires the old executor to stop. A delayed file cannot know about later server
revocation without another exchange; the coordinator rechecks current credentials,
roles, epochs and grant generations when relaying requests. Backup restore remains
quarantined under the existing stopped-authority and access-review procedure.

## Acceptance checks

- Native CPU tests: reservations exclude other workers; idempotent and conflicting
  imports; canonical manifests; exact union and verified results; outbox paging;
  elapsed delivery time, reboot and rollback; stale generations and revocation.
- Process-exit tests around durable export and import commits, plus exclusive
  file publication, malformed/oversized files and checksum rejection.
- Live localhost mTLS courier checks, followed by tiny xpoint/BSGS executions on
  both MI300X and H200 with no network child on the offline worker.
- Existing storage/coordinator recovery and supervised worker regressions, focused
  host sanitizer checks, documentation and artifact validation.

The [acceptance record](C22_VALIDATION.md) links raw results and reproduction details.

## File codec validation

The native gate, `coordinator_offline_files`, passes on the CPU release
build. It checks exact JSON/checksum round trips, preservation of an existing
destination, invalid checksums, duplicate JSON fields, size bounds, public
permissions, symlinks, hardlinks and nonblocking rejection of a FIFO. Failed
publication leaves no staging file. Linux `renameat2(RENAME_NOREPLACE)` provides
exclusive publication without a transient second hardlink.

## Durable transfer and reconciliation validation

Schema v7 adds a delivery-attempt ledger referencing the existing immutable worker
requests. Migrations v1–v6 remain unchanged. Export commit precedes publication;
import atomically updates grants, outbox acknowledgments and the response digest.
A retry returns the same pending file. Explicit refresh or a changed boot retires
the old attempt while preserving its machine request. Accepted/denied response
hashes remain retained, so old duplicates cannot mutate later state.

The authenticated `POST /api/v1/offline-sync` endpoint reuses machine sync and
rechecks current grant ownership/generation/expiry before returning even a cached
receipt. Definite denials pause imported work without deleting pending results.
The file-only configuration can omit credential paths; keys remain on the courier.
It rejects the direct HTTPS sync interface.

The [final validation](C22_VALIDATION.md#validation) includes CPU release, debug,
sanitizer and native HIP/CUDA gates with actual localhost TLS. Native cases cover disjoint reservations during file transit, complete ten-block union
and four verified matches, foreign/conflicting/duplicate imports, invalid manifests,
elapsed delivery time, superseded attempts, reboot, 130-checkpoint paging,
checkpoints arriving after export, credential revocation, recovered generations
and expired cached grants. The v6 upgrade retains one sealed v6 snapshot.
A regressed local monotonic clock rejects both export retry and import without
changing the pending transfer. Relaying that same machine request later retains
its original server expiry and consumes elapsed time in the local deadline.
Four child-process exit points around export/import COMMIT verify recovery;
import faults include pending result/coverage outbox deletion in the transaction.

## User commands

`keyhunt-worker configure` accepts `"transport":"file"` for a new worker. Its
configuration requires the coordinator HTTPS authority and job/device queues;
credential fields can be omitted. Existing HTTPS configurations default to
`"transport":"https"` and cannot be changed in place.

| Command | Required options | Effect |
| --- | --- | --- |
| `file-export` | `--state-dir DIR --output FILE` | Persist/reuse the pending request and publish its portable envelope; prints transfer ID and SHA-256 |
| `file-export --refresh yes` | Same | Supersede the previous delivery attempt, retaining the immutable machine request; old responses fail import |
| `file-relay` | `--config COURIER.json --input REQUEST --sha256 HASH --output RESPONSE` | Verify the transferred request and exchange it over mTLS; no worker journal is opened on the courier |
| `file-import` | `--state-dir DIR --input RESPONSE --sha256 HASH` | Apply a matching response atomically; exact duplicates return `duplicate:true` without changing state |

A courier configuration contains `endpoint`, `ca`, `certificate`, `key`, and an
optional localhost `resolve` override. Its authority must match the request.
Request/response files must be absolute paths in an existing private directory
outside every checkout. Destinations must not exist. Keep the printed SHA-256
in a trusted channel when transferring a file. A digest copied from the same
untrusted source as the file does not authenticate it.

Definite HTTP 401/403/404/409/426 responses become bound denial files. A successful
`file-import` command means the file was applied, not necessarily that work was
authorized: inspect its `status` (200 for acknowledgment) and worker pause state.
Transport failures and transient server errors produce no response file.

The supervisor reads the immutable transport setting. In file mode it creates
no scheduled-sync child or sync log, while retaining GPU owners, local controls
and failure isolation. Explicit `sync`, `scheduled-sync` and `api` calls are
rejected on that worker. `file-relay` is deliberately a separate courier action.
The CPU localhost CLI gate covers real mTLS, wrong CA/authority and checksum
rejection, exclusive file publication, duplicate imports, credential revocation
and reviewed reactivation for both xpoint and BSGS jobs. The supervisor fixture
also verifies two concurrent file-only owners without a network child.

## Run a disconnected worker

Build with `KEYHUNT_ENABLE_COORDINATOR=ON` on the GPU host and use the native
HIP or CUDA binaries. A CPU coordinator build with the HTTPS worker enabled is
sufficient on the connected courier. Both still need their normal linked runtime
dependencies; file mode disables contacts, not the libcurl build dependency.
Prepare an authorized job and worker certificate using
[the coordinator setup](COORDINATOR.md#s06-isolated-localhost-operation). That demo
prints the project/job identifiers and generates a usable courier configuration
in `alice-worker.json`; keep its CA confined to the localhost environment.

On the disconnected GPU machine, create a new private directory outside Git and
save this configuration with the actual authority, project and job values:

```json
{
  "transport": "file",
  "endpoint": "https://COORDINATOR_HOST:443",
  "jobs": [{
    "project": "PROJECT_UUID",
    "job": "JOB_SHA256",
    "devices": ["0", "1"],
    "spares": 1,
    "policy": "sequential"
  }]
}
```

Use only the device queues you intend to run. `random` and `random-window` select
unexplored blocks; their ranges and widths remain immutable. Use a fresh journal
for this file-only worker, rather than copying another machine's active journal.
The worker instance is generated and persisted locally during configuration.

With installed binaries on PATH, set `offline_state` to that new private journal
directory, `offline_config` to the configuration file, and `exchange_dir` to an
existing private exchange directory. Then run on the GPU machine:

```sh
keyhunt-worker configure --state-dir "$offline_state" --config "$offline_config"
keyhunt-worker file-export --state-dir "$offline_state" \
  --output "$exchange_dir/request-001.json"
```

Transfer the request file to the connected courier and obtain its printed digest
through the trusted transfer channel. The courier configuration holds the normal
`endpoint`, `ca`, `certificate` and `key` paths for the enrolled worker credential.
Set `courier_config` and `exchange_dir` for that machine, then substitute the
trusted request digest below:

```sh
keyhunt-worker file-relay --config "$courier_config" \
  --input "$exchange_dir/request-001.json" --sha256 REQUEST_SHA256 \
  --output "$exchange_dir/response-001.json"
```

The relay's printed `status` is 200 for an assignment/acknowledgment response.
The coordinator has already committed its reservations before this file is
published. A lost file or stdout message does not undo that transaction. Relay
the same saved request to a **new output filename** if delivery was uncertain.
Reusing its machine request does not reserve another set of blocks.

Transfer the response and its trusted printed digest back to the GPU machine:

```sh
keyhunt-worker file-import --state-dir "$offline_state" \
  --input "$exchange_dir/response-001.json" --sha256 RESPONSE_SHA256
keyhunt-worker status --state-dir "$offline_state"
```

Inspect `status` and any `pause_reason` before executing. With acknowledged,
unexpired grants, set `backend=hip` or `backend=cuda` and run:

```sh
keyhunt-supervise --state-dir "$offline_state" --backend "$backend" \
  --worker "$(command -v keyhunt-worker)" --keyhunt "$(command -v keyhunt)" \
  --devices 0,1 --once
```

From a checkout use `python3 tools/coordinator_worker.py` and explicit build
binary paths instead of installed names. For BSGS also pass `--table FILE` with
the canonical versioned table used to create the job. File responses contain its
configuration/checksum and canonical targets, not the potentially large table.
After visibility changes use `--device-map QUEUE=ORDINAL`; normal UUID rebind,
local pause controls and stopped-owner requirements remain in force.

Export again to a new filename after checkpoints or local completion:

```sh
keyhunt-worker file-export --state-dir "$offline_state" \
  --output "$exchange_dir/request-002.json"
```

Repeat relay and import. The same exchange carries checkpoint pages, results,
lease renewals and replacement reservations. Only imported acknowledgments clear
the exported outbox page; local completion alone leaves the server block in
progress. One request carries at most 64 checkpoints/4,096 match observations;
repeat exchanges until `outbox_bytes` is zero. New local checkpoints can arrive
during file transit and remain pending for the following exchange.

## Recovery and operational limits

- Re-exporting a pending attempt on the same boot reproduces its exact contents.
  If a response is delayed, its lease still runs from the original export origin;
  importing it later never grants a fresh thirty days.
- `file-export --refresh yes` starts a new delivery attempt and invalidates older
  response files. The pending machine payload and exported outbox page are retained.
  A changed Linux boot automatically starts a fresh attempt. Send that new request
  to the coordinator before resuming uncertain grants.
- An exact already-imported response is a no-op, even after later exchanges or a
  reboot. A conflicting response for that transfer is rejected. Retired files and
  transfer history are retained for audit; no automatic history archival is added.
- Current revocation or stale-generation refusals pause work when their denial
  file is imported. They cannot stop a disconnected GPU before delivery. Stop the
  old owner and withdraw its old journals/outstanding files before handing work to
  a replacement; do not resume them after transfer. Cached relay responses are
  rechecked against current assignment generations and expiry at the coordinator.
- Expired work needs explicit [coordinator recovery](COORDINATOR.md#s05-recovery-and-coordinator-only-installation).
  Files cannot extend leases locally or activate a quarantined backup. A stale
  immutable request may remain refused; recover into a fresh worker instance after
  stopping the old one, rather than editing grants or SQLite rows.
- Pending results consume the existing bounded outbox. If it fills, execution
  stops without discarding data. Exchange and import pages to free space. Keep
  exchange artifacts private: result files can contain found private scalars.

This implementation uses trusted manual transfer plus digest pinning, not a new
signing-key infrastructure or encrypted file format. Live integration validates
localhost mTLS; physically moving files between separate hosts and public ingress
remain deployment follow-up checks. Normal HTTPS workers retain their two-hour
schedule. File-only workers report `sync_due_in:null`, contact no server automatically,
and require the operator to arrange exchange before their saved deadlines expire.

## Reproduce the integration gate

Use the build's explicit Apache root and choose `--backend cuda` for NVIDIA:

```sh
python3 tests/coordinator/offline_cli.py \
  --coordinator build/hip-release/keyhunt-coordinator \
  --worker build/hip-release/keyhunt-worker --keyhunt build/hip-release/keyhunt \
  --apache-root / --hardware --backend hip --device 0 \
  --report /var/tmp/keyhunt-offline-hip.json
```

Omit `--hardware` for the CPU transport gate. The hardware gate reserves two blocks
per mode, stops both services during execution, checks exact local coverage and
scalar-1 matches, then restarts the services and verifies final acknowledgment.
It also checks retained executors across grants and absence of a network child.
Run these correctness checks independently of performance measurements.

## C23 HASH160 family

The same manual exchange supports mode `hash160`, including separate compressed
and uncompressed target relations. Updated workers require the coordinator's
`hash160-v1` capability support before grants can be imported. Both native GPU
owners preserve exact coverage, both encoding results, bounded outbox state and
one prepared executor across active/spare handoffs while disconnected. The
[mode contract and acceptance](C23_HASH160.md) describe this extension; C22's
historical evidence above remains scoped to xpoint/BSGS.

## C23 Ethereum family

Mode `ethereum` uses the same file export/relay/import workflow and saved lease
rules. Update both coordinator and worker for `ethereum-v1` negotiation. The
canonical 20-byte address binding survives disconnected execution, local results,
subsequent upload and duplicate acknowledgment import. Both native backends are
covered by the [Ethereum acceptance](C23_ETHEREUM_VALIDATION.md); the original C22
archive remains an unchanged record of its earlier release.
