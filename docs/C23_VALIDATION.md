# C23 first family: Bitcoin P2PKH and HASH160 acceptance

The user-selected Bitcoin mainnet P2PKH / raw HASH160 family is complete on
MI300X HIP and H200 CUDA. This is the frozen first-family acceptance record.
The subsequent [Ethereum family](C23_ETHEREUM_VALIDATION.md) is also complete.
**C23 remains partial:** the later [vanity acceptance](C23_VANITY_VALIDATION.md)
completes P2PKH prefixes; minikeys and other GPU mode families remain pending. See the
[contracts and decisions](C23_HASH160.md) and [runnable quickstart](GPU_QUICKSTART.md).

## Implementation and findings

Separate commits introduce canonical 21-byte encoding/hash targets, strict
Base58Check loading, independent GPU SHA-256/RIPEMD-160 primitives, direct and
four-scalar stepped kernels, verified checkpoint replay and coordinator support.
Comments explain byte order, normalization, buffer bounds, relation identity,
commit boundaries and capability fencing. Legacy `-m` commands retain CPU behavior.

Encoding is part of the target identity. Both encodings may match at one scalar;
the unique durable relation is the scalar/target pair. Candidate allocation uses
scalar count times enabled encodings, with no assumed hash-preimage bound.
An overflowing attempt credits nothing and replays smaller intervals. Every
retained candidate is independently verified on the CPU before persistence.

Mode 3 uses the existing schema-v7 columns and checkpoint encoding. No published
migration changed. Unknown modes fail explicitly. The new `hash160-v1` capability
is required before HASH160 reservation or cached receipt replay. Updated servers
continue accepting old workers for xpoint/BSGS; updated workers require the
updated coordinator. Addresses are an input format; wire jobs carry canonical
tagged hashes.

Two related ownership/identity findings were fixed separately:

- The [terminal teardown deadline](TERMINAL_EXIT_RECOVERY.md) now remains bounded
  after a device emits a terminal event but hangs during destruction. Tests cover
  a stuck child, healthy-peer progress, quarantine and shared cleanup budgets.
- Adding another scalar-search label required an explicit xpoint algorithm check
  at both submission and CPU verification. Cross-mode negative tests and the
  old ownership/fault suites pass on both native backends.

The independent RIPEMD-160 oracle needed an OpenSSL legacy-provider fallback on
these hosts; the bulk fixture generator uses OpenSSL's independent low-level
implementation when hashlib lacks that provider. Production hashing does not
change provider policy. Published empty/`abc` vectors verify the test adapter.

## Evidence and validation

[The acceptance manifest](baselines/C23_VALIDATION.json) records source phases,
source/binary hashes, hardware inventories, limits and artifact hashes.
[Algorithm reports](baselines/C23_ALGORITHMS.json),
[recovery reports](baselines/C23_RECOVERY.json),
[offline worker reports](baselines/C23_OFFLINE.json),
[executable examples](baselines/C23_EXAMPLES.json) and
[raw logs](baselines/C23_LOGS.tar.gz) retain detailed evidence. No private keys,
credential contents or journals are archived.

| Gate | Result |
| --- | --- |
| Relevant CPU release regressions | 33/33 pass: storage, checkpoint, coordinator, offline, supervisor, planner and HASH160 |
| New host Debug / ASan+UBSan gates | 4/4 each; the later xpoint identity sanitizer check also passes |
| Independent short-message hashes | 667 vectors per native backend, plus portable host and sanitizer runs |
| Native HASH160 owner/fault tests | HIP and CUDA pass every-index tails, ownership/tickets, two-encoding replay and 31 failure boundaries |
| Independent CLI search corpus | 48 cases and 46 rejection checks per kernel per backend; direct and stepped both pass |
| Large and boundary ranges | High-bit carries, curve-order tails, no-match, dense overflow and 1,048,576-scalar batches covering all 20 cached offset bits |
| Device coverage | Standalone search fixtures exercise all eight MI300X and all eight H200 ordinals |
| Durable native recovery | Both hosts pass changed-input rejection, interrupted-process replay, pause/resume, snapshots, changed visibility and finished retries |
| Authenticated online/offline owners | Both hosts pass two HASH160 grants with one prepared executor; both encoding results reconcile exactly |
| CUDA context isolation | Passes after adding both HASH160 self-test kernels, and after the identity correction |
| Operator examples | CPU preparation and full HIP/CUDA quickstarts pass with document, script and binary digests |

The H200 checkout is isolated under `/var/tmp/keyhunt-c23/source`, with binaries
in `/var/tmp/keyhunt-c23/build`. It has full Git history. An initial bundle clone
needed an explicit `main` checkout. One direct CLI run was interrupted when a
later build replaced its executable; the recorded `PermissionError` was a harness
scheduling error. The complete direct corpus then passed against a stable build.
The archive keeps that initial log and the passing follow-up. Phased reports
retain their actual binary hashes; they are not presented as runs of one binary.

## Scope limits

Address input supports mainnet P2PKH only. P2SH, witness, testnet and Ethereum
inputs fail. The tested hosts are eight MI300X gfx942 SPX/NPS1 devices and eight
H200 sm_90 devices with MIG disabled. This adds no partition/MIG certification.

These are correctness and recovery checks, not HASH160 throughput or scaling
measurements. HASH160 block width remains an explicit operator choice; the C20
fleet calibration harness remains scoped to xpoint/BSGS. Localhost mTLS was used
for coordinator acceptance; no production service or public endpoint was deployed.
