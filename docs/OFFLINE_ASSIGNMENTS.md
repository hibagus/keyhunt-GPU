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
