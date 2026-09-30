# C22 offline assignment files

Status: implementation in progress. The existing authenticated sync protocol is
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

## Acceptance plan

- Native CPU tests: reservations exclude other workers; idempotent and conflicting
  imports; canonical manifests; exact union and verified results; outbox paging;
  elapsed delivery time, reboot and rollback; stale generations and revocation.
- Process-exit tests around durable export and import commits, plus exclusive
  file publication, malformed/oversized files and checksum rejection.
- Live localhost mTLS courier checks, followed by tiny xpoint/BSGS executions on
  both MI300X and H200 with no network child on the offline worker.
- Existing storage/coordinator recovery and supervised worker regressions, focused
  host sanitizer checks, documentation and artifact validation.

Commands, raw evidence and final acceptance will be added with the implementation.

## File codec validation

The first native gate, `coordinator_offline_files`, passes on the CPU release
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

CPU release validation passes all three new offline gates and 26 focused
storage/coordinator regression gates, including actual localhost TLS. The new
cases cover disjoint reservations during file transit, complete ten-block union
and four verified matches, foreign/conflicting/duplicate imports, invalid manifests,
elapsed delivery time, superseded attempts, reboot, 130-checkpoint paging,
checkpoints arriving after export, credential revocation, recovered generations
and expired cached grants. The v6 upgrade retains one sealed v6 snapshot.
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

File-only status reports `sync_due_in:null`, making the lack of automatic contacts
explicit. The supervisor handles that value without creating a network child.
Native offline, fault, live CLI and two-owner supervisor gates pass this contract.
