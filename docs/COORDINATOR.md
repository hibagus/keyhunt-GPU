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
