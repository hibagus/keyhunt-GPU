# C23 Bitcoin vanity prefix acceptance

The Bitcoin P2PKH vanity family is complete on MI300X HIP and H200 CUDA. Native
`vanity` searches, durable checkpoints and authenticated/offline workers exhaust
exact scalar intervals and preserve every matching prefix/encoding relation.
C23 remains partial: minikeys and other candidate mappings need separate designs
and parity gates. Bitcoin [HASH160](C23_VALIDATION.md) and
[Ethereum](C23_ETHEREUM_VALIDATION.md) retain their separate evidence.

## Behavior and decisions

The [contract](C23_VANITY.md) defines case-sensitive Bitcoin mainnet P2PKH
prefixes of 1..34 Base58 characters, beginning with `1`. The GPU constructs the
complete Base58Check address before comparing prefixes, including leading zeroes
and checksum characters. This avoids approximate HASH160 interval matching.
Legacy `-m vanity` retains its existing CPU behavior and flags.

Each canonical target contains an encoding tag, prefix length, ASCII text and
zero padding, for exactly 36 bytes. Encoding, length and case remain immutable
job identity. Overlapping `1`, `1B` and `1Bg` all produce separate relations when
they match an address. Full-address prefixes are accepted. Duplicate input lines
deduplicate; matching a target does not end a search early.

At most one prefix of each length can match an address. The conservative output
bound is the sum of distinct lengths for each encoding, at most 68. Capacity
must hold that many candidates for one scalar. An overflowing attempt credits
no coverage and discards all candidates before replaying a smaller interval.
Thousands of disjoint prefixes therefore do not require thousands of slots per
scalar. GPU length masks skip lengths absent from the canonical set.

CPU verification uses retained SHA-256/RIPEMD-160 and libbase58. The GPU uses
portable hash primitives and a separate fixed-storage Base58 conversion.
Independent tests combine pinned libsecp256k1 public points with Python integer
Base58 and hashlib/OpenSSL. The all-zero hash vector verifies leading-zero
serialization independently of whether such a hash has a known scalar preimage.

Work algorithm 4 and journal mode 5 distinguish vanity from earlier families.
The existing scalar checkpoint owner provides pause, fencing and exact complement
replay; schema 7 and published migrations are unchanged. Coordinators require
`vanity-v1` before allocation, renewal, update or cached receipt replay. Older
capability lists remain valid for their previously supported families. Owners
run both vanity kernels in the per-device startup self-test and keep targets and
GPU allocations across grants. Deploy the coordinator before updated workers.

## Validation evidence

The [acceptance manifest](baselines/C23_VANITY_VALIDATION.json) records source
phases, binary/source hashes, device inventories and artifact checksums. Earlier
algorithm binaries have their own embedded hashes; they are not represented as
identical to the later worker build.

| Gate | Result |
| --- | --- |
| CPU release | 36 relevant contract, checkpoint and coordinator checks pass |
| ASan/UBSan | 23 relevant host checks pass; the expanded overlapping coordinator case also passes separately |
| Base58Check oracle | 345 portable/native cases per backend, including leading zeroes and invalid hash lengths |
| Native searches | 50 independent cases and 31 rejections per kernel per backend; all eight visible ordinals |
| Executor ownership/faults | Direct/stepped receipts, stale/foreign tickets, dense tails and 31 injected boundaries per backend |
| Prefix overlap | Both encodings, case distinctions, full-address/checksum prefixes, and 68 simultaneous relations at one scalar |
| Exact ranges | Multi-limb carry and curve-order tails; maximum 1,048,576-scalar batch with every cached offset bit |
| Recovery | Nine native cases per backend: both kernels, dense overlap, no-match, carry/order, kill after acknowledgment and changed geometry |
| Pause/restart | Repeated pause/resume, graceful signals, backup quarantine and changed device visibility |
| Worker transport | HTTPS and disconnected file exchange, two grants with one executor, later acknowledgment and empty outbox |
| Server relations | Sixteen overlapping prefix/encoding relations survive two grants and a lost upload acknowledgment |
| Regression | Existing xpoint/HASH160/Ethereum executors and fault gates, shared checkpoints and CUDA context isolation |
| Documentation | CPU, HIP and CUDA execute the marked quickstart; six volatile commands and five durable families |

[Algorithm reports](baselines/C23_VANITY_ALGORITHMS.json),
[recovery reports](baselines/C23_VANITY_RECOVERY.json),
[offline reports](baselines/C23_VANITY_OFFLINE.json),
[executable examples](baselines/C23_VANITY_EXAMPLES.json) and
[raw logs](baselines/C23_VANITY_LOGS.tar.gz) retain detailed observations.
Only public scalar fixtures were used. Credential contents and journal databases
are excluded from the archive.

A recovery fixture initially tried to claim the same canonical job once per
kernel. Since kernel choice deliberately does not change job identity, the
second claim correctly found no unassigned block. The fixture now isolates each
kernel's grants in a separate project. Production semantics were unchanged.
An early local recovery invocation overlapped the final build; it was interrupted
and replaced by the complete post-build run retained in the acceptance archive.

## Reproduction and limits

Build using [BUILD.md](BUILD.md). Run
`ctest --test-dir BUILD -R 'vanity|base58' --output-on-failure`,
followed by the shared checkpoint, HTTPS/offline and context
checks named in the manifest. The shared regression suites also exercise Ethereum
and require its [pinned test oracle](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits).
The [quickstart](GPU_QUICKSTART.md) includes overlapping vanity prefixes and
completed-grant replay.

Validation used eight visible SPX/NPS1 MI300X devices and eight MIG-disabled H200
devices. Sequential ordinal coverage does not establish vanity multi-device
scaling. This delivery makes no new throughput, tuning, calibrated block width,
partition/MIG certification or public coordinator deployment claim. Regex,
case-insensitive, suffix, Ethereum, testnet, P2SH and witness prefix searches are
outside the contract.
