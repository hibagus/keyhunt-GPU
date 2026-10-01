# C23 minikey acceptance

The agreed 22- and 30-character minikey family is complete on MI300X HIP and
H200 CUDA: exact candidate search, both public-key encodings, durable recovery
and authenticated/offline workers. Other candidate mappings require their own
parity gates; this acceptance does not make a blanket claim for all legacy modes.

## Behavior and decisions

The [contract](C23_MINIKEYS.md) defines a one-based, fixed-width Base58 ordinal
space. Ordinal 1 maps to `S` followed by all `1`s. Every candidate is tested,
including those rejected by the SHA-256 check byte. There is no probabilistic
skip or increment-before-test. Accepted candidates derive a private scalar using
SHA-256 without reduction modulo the curve order. The direct kernel then checks
Bitcoin P2PKH/HASH160 targets for the selected encodings.

A canonical target contains length, encoding and HASH160 in 22 bytes. This binds
length and encodings to immutable job identity. Address and raw-hash inputs are
equivalent. Matching a target does not end the assigned interval early. Output
capacity must fit one candidate's encoding relations; overflow discards the
whole attempt and replays a smaller ordinal interval with zero partial credit.

Adjacent candidate strings derive unrelated private scalars, so a stepped
point-walk kernel cannot preserve these semantics. Only `direct` is supported.
CPU verification reconstructs each candidate and checks admission and its full
public-key relation independently. The oracle suite combines Python integers and
hashlib, pinned libsecp256k1 and OpenSSL HASH160.

Work algorithm 5 and journal mode 6 distinguish ordinals from private scalars.
The bounded integer scheduler can represent both 22- and 30-character spaces;
`ordinal_at` rejects use for other work algorithms, and `scalar_at` rejects
minikey work. Internal receipt/SQLite fields historically named `scalar` contain
the ordinal in mode 6. Public results expose `ordinal`, `minikey` and the derived
`scalar` separately. Summaries use `computed_ordinals`/`resumed_ordinals` and
`coordinate_space:"minikey-ordinal-v1"`. Schema 7 and published migrations remain
unchanged.

Coordinators require `minikeys-v1` before allocation, renewal, updates or cached
receipt replay. Prior capability lists retain support for their original modes.
Device owners test both minikey lengths before accepting grants and retain their
targets and allocations across block handoff. The supervisor omits an unspecified
kernel override so the owner can choose the mode default. An explicitly stepped
minikey request is rejected.

## Validation evidence

The [manifest](baselines/C23_MINIKEYS_VALIDATION.json) binds source phases,
compiler settings, binaries, inventories and artifact checksums. Search reports
retain the earlier algorithm binary hashes; they are not represented as the
later integrated worker binaries.

| Gate | Coverage |
| --- | --- |
| CPU release | Full 73-test suite; the corrected offline fixture passes its separate rerun |
| ASan/UBSan | 36 host contract, storage and coordinator checks |
| Mapping/SHA oracle | 1,781 portable/native vectors, 12 admitted examples, both lengths, Base58 and limb carries |
| Native searches | 70 cases, 32 rejections and four CPU inspections per backend; all eight visible GPU ordinals |
| Exhaustive large batches | 1,048,576 ordinals per length; every admitted candidate's two encoding relations checked |
| Admission observations | 4,193 valid 22-character and 4,261 valid 30-character candidates in those fixture intervals |
| Executor and faults | Ownership, stale/foreign tickets, tail bounds, poisoned state, 29 injected boundaries per backend |
| Durable recovery | Eight native cases per backend, including both lengths, overflow, rejected-only work, final ordinal and process death |
| Pause/restart | Both lengths, repeated pause/resume, graceful signals, backup/restore quarantine and changed visibility |
| Coordinator | Capability fencing, length/domain rejection, two-grant completion, lost upload acknowledgment and idempotent replay |
| Worker transports | Localhost HTTPS and disconnected file exchange; one executor across two grants and later empty outbox |
| Regression | Prior native executor/fault gates, shared checkpoints, supervisor and CUDA context isolation |
| Documentation | CPU/HIP/CUDA execute the marked quickstart: eight volatile commands, seven durable jobs and completed-grant retries |

[Algorithm reports](baselines/C23_MINIKEYS_ALGORITHMS.json),
[recovery reports](baselines/C23_MINIKEYS_RECOVERY.json),
[offline reports](baselines/C23_MINIKEYS_OFFLINE.json),
[executable examples](baselines/C23_MINIKEYS_EXAMPLES.json) and
[raw logs](baselines/C23_MINIKEYS_LOGS.tar.gz) retain the detailed observations.
Only published public minikey fixtures were used; the archive excludes credential
contents and journal databases.

During development, the command dispatch whitelist needed the new command in
addition to its handler. Two integration fixtures also needed distinct names
for the ordinal, pause timestamp and private-file helper. The corrected fixtures
pass their reruns. An initial pause invocation lacked the existing Ethereum test
oracle environment; the recorded acceptance uses the configured environment.
The quickstart checker was also corrected to read volatile coordinate labels
from stream metadata and durable labels from standalone result rows. A final
API boundary check classifies out-of-domain minikey ranges as HTTP 400 before
persistence, instead of reporting temporary coordinator unavailability. The
focused coordinator regression passes on release, sanitizers, HIP and CUDA. These
findings do not change the candidate mapping or recovery contract.

## Reproduction and limits

Build using [BUILD.md](BUILD.md), then run
`ctest --test-dir BUILD -R minikey --output-on-failure` and the shared checkpoint,
HTTPS/offline, supervisor and backend regression checks named in the manifest.
Shared tests also exercise Ethereum and require its
[pinned oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits).
The [quickstart](GPU_QUICKSTART.md) provides both public examples and derives
range starts using the CPU-only inspection helper.

Validation used eight SPX/NPS1 MI300X devices and eight MIG-disabled H200s.
Sequential device coverage does not establish multi-device scaling. No new
throughput, tuning, calibrated-width, partition/MIG certification or public
coordinator deployment claim is made. Length 26, PBKDF2, custom alphabets and
random candidate traversal are outside this slice. Legacy `-m minikeys` retains
its original CPU behavior and flags.
