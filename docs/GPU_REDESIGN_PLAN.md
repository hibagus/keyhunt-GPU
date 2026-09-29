# Keyhunt GPU redesign plan

Status: proposed implementation plan; this change adds documentation only.
Prepared: 2026-09-29.
Main repository baseline: `2134a20` (`main`).
Reference repository baseline: `/home/bagus/keyhuntM1CPU` at `f80e95e`.

Server refinement: the [coordinator server plan](COORDINATOR_SERVER_PLAN.md)
specifies externally stored SQLite, project permissions, Apache HTTPS on TCP 443,
and per-client certificate authentication. It is the detailed deployment design
for sections 7–8 and implementation milestone C15.

## 1. Intended outcome

Keep the existing CPU functionality while separating the application, arithmetic,
device kernels, and work management. Deliver AMD HIP first on the available
MI300X system, followed by a tested NVIDIA CUDA backend sharing the same search
semantics. Provide durable pause/resume, automatic or manual work selection, and
coordination across computers. Optimize measured bottlenecks, including optional
architecture-specific assembly, without weakening coverage or result correctness.

This document specifies future behavior. Proposed commands, file paths, schemas,
and performance targets below are not features of the current executable.
Implementation should proceed as individually reviewable commits in section 10.

| Requirement | Design and delivery sections |
| --- | --- |
| Organize source, headers, kernels | 3; commits C02–C04 |
| Redesign README using the requested template | 9; C03 and C21 |
| AMD HIP and NVIDIA GPU implementations | 4; C07–C11 and C18 |
| Performance, including assembly | 5; C16–C20 |
| Pause and resume | 7; C13–C14 |
| Distributed blocks and efficient journaling | 6 and 8; C05, C12, C15, C22 |

## 2. Findings from the current checkouts and host

### Main repository

- The working tree was clean at inspection. There is no CMake build or GPU
  backend in this checkout. The root Makefile builds `keyhunt`, `legacy`, and
  `bsgsd` with repeated source lists and x86-specific optimization flags.
- `keyhunt.cpp` is 6,689 lines combining argument parsing, global configuration,
  file formats, target loading, CPU workers, BSGS, and result writing.
  `keyhunt_legacy.cpp` and `bsgsd.cpp` add separate large implementations.
- CPU arithmetic and hashes contain x86 assembly/SSE. These should remain host
  implementations; moving them into a GPU translation unit is not a port.
- Existing modes include `address`, `rmd160`, `xpoint`, `bsgs`, `pub2rmd`,
  `minikeys`, and `vanity`, with Bitcoin/Ethereum and compression options.
- The `-S` BSGS files are precomputation caches, not search progress checkpoints.
  The function named `checkpointer` checks allocations, not recovery state.
- [BSGSD.md](../BSGSD.md) describes a daemon that keeps BSGS tables in memory and
  services range requests. It is not a durable distributed range coordinator.
- The root license is MIT, with additional licenses in vendored directories.
  Preserve notices and record provenance during moves or reference-code reuse.
- The existing `tests/` directory primarily contains input fixtures. Establish
  automated correctness and recovery tests before changing search behavior.

### Reference repository: useful starting points, not a validated GPU engine

The reference contains CMake configuration, ARM/NEON CPU work, candidate core
interfaces, and two CUDA files. Inspection found:

| Location in `/home/bagus/keyhuntM1CPU` | Finding and consequence |
| --- | --- |
| `CMakeLists.txt`; `keyhunt.cpp` | A CUDA library is linked, but searches for `cudaInit` and `cudaLaunchBSGS` found definitions only in the CUDA file and no CLI calls. Establish an actual backend execution path and test it. |
| `cuda/bsgs_kernel.cu:82` | `startOffsetLimbs` is passed but never consumed. Nonzero range starts require explicit mathematical treatment. |
| `cuda/bsgs_kernel.cu:171` | Only the first 1,024 candidates are stored; excess candidates are dropped. Overflow must invalidate completion and trigger replay. |
| `cuda/bsgs_kernel.cu:172` | A 64-bit giant-step index is cast to 32 bits. Define absolute scalar and local index widths separately. |
| `cuda/secp256k1.cuh:179` | Reduction multiplies a 32-bit limb by a 33-bit constant in a 64-bit temporary. Audit overflow and carry propagation against an independent oracle before reuse. |
| `cuda/secp256k1.cuh:316,419` | In-place doubling writes the output Y before reading the input Y to produce Z; scalar multiplication calls this in place. Replace or repair arithmetic under aliasing tests. |
| `include/keyhunt/core/distributed.h` | Coordinator declarations and in-memory work structures are present; inspection found no `WorkCoordinator::` implementations. This is not a durable scheduling service to import. |
| ARM hash sources and build configuration | The inspected Apple Silicon implementation is CPU/NEON work. No Metal GPU backend was found. |

Reuse ideas and individually verified code, with attribution. Do not copy the
reference wholesale or assume HIPIFY will fix algorithmic defects.

### Verified local environment

Read-only inspection observed:

| Probe | Observation |
| --- | --- |
| `/opt/rocm/core-10.0/.info/version` | `10.0.0` |
| `hipcc --version` | HIP `7.15.26333-0000000`; AMD clang `23.0.0git`; compiler under `/opt/rocm/core-10.0/lib/llvm/bin` |
| `rocminfo` | 64 MI300X GPU agents, `gfx942`, each reporting 38 CUs and wavefront size 64 |
| `amd-smi static --partition --vram --gpu 0 --json` | Agent 0 reports CPX compute partitioning, NPS4 memory partitioning, and CAPPING allocation mode |
| `cmake --version` | `3.22.1` |
| `command -v nvcc` | No NVIDIA compiler found on the current PATH |

Treat these as installed component versions, not interchangeable version numbers.
The 64 agents are logical devices; do not report them as 64 physical MI300X cards.
Map UUIDs/BDFs to physical packages and query HIP allocatable memory per agent
before sizing tables. The SMI VRAM figure alone is not a per-worker memory budget.
Partition-aware profiling is necessary; AMD documents how partition mode changes
the scope of counters and resources. [AMD partition profiling guide][partitions]

No CPU baseline benchmark, GPU correctness test, or throughput measurement was
performed for this planning change. Availability of the tools and devices is
verified; successful compilation and runtime compatibility remain C01/C07 gates.

## 3. Repository and build architecture

### Proposed layout

```text
keyhunt-GPU/
├── CMakeLists.txt
├── CMakePresets.json
├── Makefile                     # temporary compatibility wrapper
├── README.md
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── cmake/                       # compiler detection and build helpers
├── include/keyhunt/             # host-facing interfaces; no HIP/CUDA headers
│   ├── core/                    # configuration, ranges, targets, results
│   ├── crypto/                  # public CPU arithmetic/hash interfaces
│   ├── backend/                 # device information and execution contract
│   ├── scheduler/               # work units, leases, coverage
│   └── storage/                 # job identity, checkpoints, journal APIs
├── src/
│   ├── app/                     # CLI, signals, status, application entry point
│   ├── core/                    # search planning and CPU result verification
│   ├── crypto/                  # maintained arithmetic/hash implementations
│   ├── backends/
│   │   ├── cpu/
│   │   ├── hip/                 # AMD runtime, allocations, streams, launchers
│   │   └── cuda/                # NVIDIA runtime, allocations, streams, launchers
│   ├── scheduler/
│   ├── storage/                 # SQLite and portable metadata encoding
│   └── coordinator/             # optional service and protocol implementation
├── kernels/
│   ├── common/                  # shared field/point/hash/search device code
│   ├── hip/                     # thin .hip entry points
│   ├── cuda/                    # thin .cu entry points
│   └── arch/
│       ├── amd/gfx942/          # isolated intrinsics/assembly specializations
│       └── nvidia/              # isolated PTX specializations by capability
├── third_party/                 # audited vendored libraries plus licenses
├── legacy/                     # opt-in legacy executable and bsgsd migration
├── tests/
│   ├── fixtures/
│   ├── unit/
│   ├── integration/
│   ├── gpu/
│   └── recovery/
├── benchmarks/                 # repeatable harnesses and machine-readable data
├── tools/                      # environment capture, journal inspection/export
├── deploy/                     # Apache/systemd templates; no live state or keys
├── docs/                       # architecture, modes, operations, benchmark notes
└── .github/workflows/           # CPU checks and explicit GPU validation jobs
```

Use private headers beside their implementation when they are not shared APIs.
Device headers stay under `kernels/`; third-party headers remain with their
upstream source. Audit ownership before deciding whether a current directory
belongs in `src/crypto/` or `third_party/`.

Make mechanical moves and include/build-path fixes one commit. Extract behavior
from the monolith in later commits. Preserve the existing fixture paths until
documentation and scripts have migrated, or provide compatibility aliases.
Keep the existing CLI and legacy target working during the transition.

### Build decisions

- Use C++17 and target-based CMake. Start with a 3.22 minimum, matching the host,
  then raise it only if a tested ROCm integration requirement demands it.
  CMake already exposes `HIP_ARCHITECTURES` in this version. [CMake reference][cmake]
- Use `Threads::Threads` for host threading, SQLite for the journal, and a pinned
  independently licensed oracle in test builds. Keep legacy GMP/OpenSSL needs
  confined to legacy targets. Select and pin the coordinator's HTTP/TLS library
  in C15; local search must not need network dependencies or build-time downloads.
- Provide CPU-only, HIP, CUDA, debug, sanitizer, and release presets. Default
  CPU builds must not require either GPU SDK. An explicitly requested backend
  without its compiler/runtime must fail configuration with a useful message.
- Compile AMD entry points with CMake's HIP language and a discovered AMD clang
  compiler; do not set `CMAKE_HIP_COMPILER` to the `hipcc` wrapper. Compile NVIDIA
  entry points with the CUDA language and nvcc. Validate toolkit discovery on
  this Core 10.0 layout before publishing command lines.
- Share portable device algorithm headers across HIP and CUDA. Runtime APIs,
  lane operations, launch code, and ISA specializations are small adapters.
  Native CUDA is the NVIDIA delivery path; HIP-on-NVIDIA is an optional future
  experiment, not a prerequisite for supporting NVIDIA.
- Initially ship separate CPU/HIP/CUDA builds with the same CLI and state
  formats. Defer loading both vendor runtimes in one process until needed.
- Set `CMAKE_HIP_ARCHITECTURES=gfx942` in the MI300X preset. Require an explicit
  validated CUDA architecture list in NVIDIA release presets.
- Keep `-march=native` and CPU SSE/NEON flags on CPU targets. Do not forward
  x86 flags, global `-Ofast`, or floating-point fast-math flags into integer
  cryptographic kernels. Scope LTO and device linking by backend/toolchain.
- Place runtime state outside the checkout: the coordinator uses
  `/var/lib/keyhunt-coordinator/`, while standalone journals and worker outboxes
  use the user's state directory. Ignore accidental runtime databases, WAL/SHM
  files, credentials, caches, builds, and results. Track migrations, deployment
  templates, and small synthetic test vectors; never live databases/private keys.

### Execution contract

The host owns configuration, target parsing, scheduling, persistence, and exact
result verification. A backend implements discovery, capabilities, preparation,
bounded submission, completion polling, result retrieval, and draining.

`WorkUnit` contains the job digest, lease/generation, exact half-open interval,
target-set identity, algorithm configuration, and a bounded local step count.
`BatchResult` contains an executed interval or explicit failure, candidates,
overflow state, timings, and execution identity. Queuing a kernel is never
evidence of completed coverage. A backend cannot mark a journal range complete.

CPU and GPU backends use the same range contract. Unsupported modes produce an
explicit error or an explicitly requested CPU fallback; startup reports the
selected backend and devices. GPU counters must prove that a GPU request actually
launched work, avoiding the reference's build-only integration problem.

## 4. GPU correctness and feature delivery

### Delivery order and compatibility

| Mode/feature | Initial GPU delivery | Later delivery |
| --- | --- | --- |
| `xpoint` | First bounded end-to-end correctness slice | Tuned point generation and lookup |
| `bsgs` | Primary MI300X feature after arithmetic validation | Multi-target/layout tuning and table generation |
| `rmd160`, Bitcoin `address` | Preserve CPU behavior initially | SHA-256/RIPEMD-160 GPU pipeline, both key encodings |
| Ethereum address | Preserve CPU behavior initially | Keccak and encoding parity |
| `pub2rmd`, `vanity`, `minikeys` | Preserve CPU behavior initially | Separate mode designs and parity gates; no blanket GPU claim |
| Existing stride/endomorphism/search orders | Remain on established CPU path until audited | Explicit coverage mapping before GPU enablement |
| Apple Silicon | Preserve useful CPU portability work selectively | Metal is outside this AMD/NVIDIA redesign |

The first GPU release has a published mode matrix. Full CPU mode preservation
does not imply that every mode is GPU-accelerated. Minikey enumeration needs its
own ordinal-to-candidate mapping before it can share scalar-range journaling.

### Arithmetic and search invariants

1. Establish independent expected results using fixed public test vectors and a
   separately pinned arithmetic oracle, such as libsecp256k1 plus big-integer
   field calculations. The existing CPU engine is a regression oracle, not the
   only authority for cryptographic correctness.
2. Implement portable device arithmetic first. Begin by measuring 8 × 32-bit
   limbs against suitable alternatives; do not assume the reference's choice
   is optimal for AMD. Specify canonical field reduction, carry/borrow behavior,
   aliasing rules, point infinity, doubling, and scalar-domain checks.
3. Test zero/one, values around the field prime and curve order, carry chains,
   high limbs, `P + (-P)`, in-place operations, and partial thread blocks.
   Distinguish field arithmetic modulo `p` from scalar arithmetic modulo `n`.
4. Use 256-bit absolute scalars and checked wider host intermediates for endpoint
   arithmetic. Kernel-local indices may be 64-bit only after bounding each
   submitted batch; widen grid-index products before multiplication.
5. For a BSGS tile `[a,b)`, start from `Q - aG` and use a fully specified baby
   table/giant-step mapping to reconstruct `k = a + i*m + j`. Check `a <= k < b`
   and verify `kG == Q`. Handle zero baby steps, infinity, final partial tiles,
   duplicate X coordinates, both Y signs, and all hash-bucket collisions.
6. A Bloom filter is only a prefilter. Define identical construction/query hash
   functions, bit order, sizes, and versions on CPU and GPU. An exact table
   lookup plus point verification confirms candidates; no false negatives are
   allowed from filtering or collision handling.
7. Version table metadata: curve, `m`, entry format, endianness, hash algorithm,
   checksum, and producer semantics. Existing `.blm`/`.tbl` files contain native
   layouts; support a validated importer or explicit rebuild, never reinterpret
   them as portable device structures.
8. Every target in a target group must be processed before its interval is
   covered. Stop-on-match may leave partial work; finding a key does not mark
   the remaining interval exhausted. Endomorphism candidates outside an assigned
   range may be reported, but cannot credit unsearched range coverage.
9. Allocate candidate buffers with explicit capacity, count, and overflow status.
   On overflow, drain safely and replay the uncommitted batch with smaller work
   or more output space. Never silently discard candidates or advance progress.
10. CPU-verify all emitted matches, deduplicate them, and durably record them
    before acknowledging the corresponding coverage. Rate-limit work submission
    if verification or persistence falls behind.

Prepare tables once per compatible job. Use explicit device allocations and
bounded batches; validate memory availability before startup. Allow table-size
reduction only through an explicit compatible search plan, not an unnoticed
change to a running job's checkpoint interpretation.

## 5. MI300X performance and assembly strategy

There is no defensible absolute throughput target before a correct GPU baseline.
The objective is the best reproducible end-to-end throughput for specified modes,
target counts, table sizes, device partitions, and durability settings.

### Measurement before specialization

Capture the commit, compiler flags, ROCm/HIP/driver versions, GPU UUID and physical
mapping, partition modes, allocatable HBM, clocks/power state, CPU/NUMA placement,
input digest, search mode, table parameters, batch sizes, and checkpoint policy.
Report table-build/upload time, warm-up, kernel time, transfers, verification,
storage overhead, and total elapsed time separately.

Use no-match exhaustive workloads and known matches at the beginning, middle,
and end. Repeat runs (initial policy: at least five after warm-up), report median
and spread, and retain raw data. Compare equivalent work and target sets.
For BSGS, report actual giant steps/s and effective scalar-range coverage/s
separately; effective coverage is not literal public-key evaluations per second.
Do not use historical README speed tables as the new performance baseline.

### Optimization ladder

| Step | Candidate work | Evidence required |
| --- | --- | --- |
| Algorithm | Point stepping, amortized starting points, batch inversion, BSGS table sizing, validated symmetry/endomorphism | Fewer expensive operations with identical interval coverage |
| Representation | Limb layouts, specialized squaring/reduction, structure-of-arrays layouts | Arithmetic parity and measured whole-kernel improvement |
| Memory | Coalescing, filter locality, read-only table placement, fewer host/device copies | Profile showing memory or transfer bottleneck reduced |
| Execution | Threads/block, points/thread, register pressure, occupancy, bounded asynchronous batches | Register/spill counts, utilization, latency, and end-to-end timing |
| Concurrency | Overlap transfer/verification with kernels; independent logical-device workers | Multi-device speedup without duplicated coverage or memory oversubscription |
| ISA specialization | Small AMD carry/multiply/reduction or hash primitives; later NVIDIA PTX variants | Differential tests, disassembly, reproducible improvement exceeding noise |

MI300X uses `gfx942` and wave64. Shared code must still query/abstract lane width,
ballots, shuffles, masks, and synchronization; never transplant CUDA's 32-lane
assumptions. HIP also documents differences in launch-bound semantics.
[HIP porting guide][hip-porting]

Profile with the installed ROCm profiling tools, checking their versions and
counter availability first. Inspect generated instructions with the matching
LLVM disassembler. Start on one logical GPU; scale within one physical package,
then across packages, recording CPX/NPS4 effects and memory sharing. Use host
NUMA affinity only after measuring the topology and transfer behavior.

### Assembly policy

- Keep portable C++ device primitives as the reference and fallback. Try compiler
  intrinsics before assembly where they expose the required instruction.
- Isolate AMD specialization under `kernels/arch/amd/gfx942/`, guarded by target
  and compiler capability checks. Document operands, carry chains, clobbers,
  register pressure, aliasing, and supported toolchain versions. Follow the
  LLVM AMDGPU ABI/code-object rules for any standalone assembly. [LLVM guide][llvm]
- Add one primitive or tightly related optimization per commit. Require the
  arithmetic suite, end-to-end coverage tests, and a paired benchmark report.
  An initial acceptance rule is at least 5% median workload improvement outside
  observed noise; justify a different threshold with the benchmark's variability.
- Retain an opt-in assembly switch until validation is broad enough for automatic
  architecture dispatch. Recheck after compiler upgrades. If it is slower or
  fragile, keep the portable path; handwritten assembly is not inherently faster.
- NVIDIA specialization is separate. Inline PTX remains subject to compiler
  lowering, so inspect generated machine code and benchmark on NVIDIA hardware.
  Follow NVIDIA's operand/constraint guidance. [Inline PTX guide][ptx]
- Whole-kernel assembly and persistent kernels are late experiments, justified
  only when profiling shows the simpler implementation cannot reach the target.
  They must preserve bounded pause latency and identical recovery semantics.

## 6. Range blocks without an enormous preallocated journal

### Separate three sizes

Use a **logical block** for human selection, a **work unit** for a lease to a
machine/device, and a **kernel batch** for GPU scheduling and checkpoint progress.
Their sizes are independent. A slow computer need not finish a logical block in
one session, and a large block can be completed by several non-overlapping leases.

For scalar-domain jobs, store the root interval as `[L,U)`, with `1 <= L < U <= n`.
For chosen logical width `W`, block `i` is:

```text
N = ceil((U - L) / W)
block(i) = [L + i*W, min(U, L + (i+1)*W)), 0 <= i < N
```

Calculate with checked wide arithmetic. Public block IDs and endpoints may exceed
64 bits. Use fixed-width 32-byte big-endian values in storage, canonical hex in
the CLI/protocol, and wider intermediates for products/ceilings. Avoid converting
IDs, endpoints, or exact progress counts through floating-point or JSON numbers.
Audit the existing CLI's endpoint convention and translate it explicitly at the
compatibility boundary; do not silently change old `-r` behavior.

Block width is immutable for a job. Work-unit boundaries are allocated lazily
inside blocks and recorded exactly; they can vary with machine speed. Choose
logical width through `--block-bits` or `--block-size` and offer a calibration
recommendation. As initial tunable defaults, target 1–5 minutes per work unit,
0.1–1 second per kernel batch, and a durable checkpoint about every 10 seconds.
These are scheduling targets, not measured host performance.

For direct scans, estimate work-unit span from measured candidates/s. For BSGS,
use measured giant-step cost, target count, table parameter `m`, and tile setup
cost. Align work to the algorithm's valid tiles or handle boundaries explicitly;
do not equate BSGS effective keys/s with direct-scan keys/s. Recalibration affects
new leases only. A tiny illustrative job `[1000,1100)` with width 32 has four
blocks: `[1000,1032)`, `[1032,1064)`, `[1064,1096)`, and `[1096,1100)`.

This resolves the block-size tradeoff: choose a stable block size that is useful
to navigate, while smaller recorded leases and checkpoints bound the time lost
on a single computer. Show estimated work-unit duration, partial-block progress,
and journal growth so users can adjust future allocations from measurements.

### Sparse exact coverage

Do not insert one row per theoretical block. Persist a root range, sparse active
leases, partial-block coverage, and coalesced completed intervals. Missing state
means unvisited. Merge adjacent completed intervals transactionally; once a block
is complete, discard redundant per-batch detail after retaining required audit
information. A run of completed block IDs can be represented by one interval.

Use an ordered interval structure with subtree counts, or a sparse hierarchical
range tree, for locating free work and aggregating coverage. Materialize nodes
only for touched regions. Shard dense regions into bounded pages with exact
bitmaps where measurement shows this saves space; 65,536 completion bits need
8 KiB before metadata. A bitmap must describe fixed coverage cells, not unstable
adaptive lease IDs. Add such a representation only behind tested format versions.

Sparse storage scales with visited fragmentation and active work, not with `N`.
It cannot promise constant space for arbitrary random subsets: one billion
independent completion bits already require about 119 MiB before indexing, and
widely scattered 256-bit intervals cost more. Publish journal size and compaction
metrics. Prefer bounded random windows when unconstrained random selection causes
excessive fragmentation; keep the statistical difference visible to the user.

### Selection and lifecycle

| Policy | Meaning |
| --- | --- |
| `sequential` | Select the lowest eligible block, then its first unleased uncovered interval |
| `random` | Select randomly from eligible unfinished blocks, then an available interval inside it |
| `random-window` | Randomize within bounded windows, improving locality and interval compaction; not globally uniform |
| `--block <id>` | Select an exact logical block; report done/busy or lease an available part |

Distinguish never-visited blocks from partial, leased, paused, and completed ones
in status output. Allow an unvisited-only selection filter, but default automatic
scheduling should revisit eligible partial work so it can finish the whole job.
After a work unit completes, continue according to the selected policy. Manual
selection can stop at that block or explicitly enable automatic continuation.

Implement random choice by rank over exact eligible counts in the sparse index,
with unbiased bounded sampling for wide integers. Do not repeatedly sample the
entire domain and hope to hit a remaining block near exhaustion, and do not use
`ORDER BY RANDOM()` over an enumerated job. Serialize concurrent selection and
lease insertion in a transaction. Persist traversal version, seed/counter state,
and the allocation itself together. Uninterrupted and resumed order can then be
reproduced for a fixed sequence of requests, while the journal remains the source
of truth for coverage. Retrying a claim request must return the original lease.

## 7. Durable storage, checkpoints, and pause/resume

### Storage choice

For distributed operation, a dedicated server owns the authoritative SQLite
journal at `/var/lib/keyhunt-coordinator/progress.sqlite`, outside the repository
and web document root. Workers use an authenticated HTTPS API; Apache never serves
the database file. Start with one database containing project-scoped records so
permissions and progress can be committed together. The
[server plan](COORDINATOR_SERVER_PLAN.md) defines the schema boundaries and roles.
Optional standalone journals and worker retry outboxes also live outside checkouts,
under the user's state directory. Configure WAL, foreign keys, busy handling,
and `synchronous=FULL` for the durable mode; batch progress transactions so fsync
does not sit in the kernel loop. SQLite WAL requires same-host users and does not
support a database shared by workers over NFS/SMB. [SQLite WAL][sqlite-wal]
`FULL` in WAL mode provides a sync at commit; validate the actual storage stack
and failure behavior rather than treating configuration alone as proof.
[SQLite synchronization documentation][sqlite-sync]

Suggested schema responsibilities, to be finalized with migration tests:

| Entity | Durable contents |
| --- | --- |
| `projects`, `project_memberships` | Opaque project UUIDs, names, active state, and client roles |
| `clients`, `credentials` | Stable client identity, registered certificate/key fingerprints, validity and revocation |
| `idempotency_records` | Project/client/operation-scoped request keys, payload digests, and committed responses |
| `jobs` | Canonical manifest and digest, schema/semantic versions, root range, mode, target digest, block width, search/stop policy, creation state |
| `coverage` | Exact disjoint completed intervals with block/target-group identity; indexed for merging and complement queries |
| `leases` | Work ID, exact interval, worker, monotonically increasing fencing generation, expiry, committed frontier, state |
| `results` | Verified candidate/target identity and a uniqueness key; committed before or atomically with dependent coverage |
| `scheduler_state` | Selection algorithm/version, seed/counter, sparse selection index, coordinator epoch |
| `events` | Transaction sequence, idempotency key, claim/progress/pause/release/complete events and compact payload |
| `exports` | Reserved offline intervals, manifest digest, assignment generation, import/revocation state |

Jobs, leases, coverage, results, exports, and events carry project scope, with
composite foreign keys and indexes. Check authenticated project membership on
every operation and inside the same transaction as each mutation. Project IDs
organize and authorize access; the job digest identifies search semantics within
a project. Neither identifier is an authentication secret.

The durable coverage and event are committed in one transaction. SQLite's WAL
provides crash recovery; `events` provides a bounded application audit trail.
Do not build a second competing filesystem journal or retain every per-key event.
Compact acknowledged event history into versioned snapshots with a documented
retention policy; exact coverage and result records remain authoritative.

### Identity and compatibility

Hash a canonical job manifest including range bounds, curve/domain, target-set
content, search mode, compression/network/hash options, stride/endomorphism
coverage rules, and logical block geometry. Normalize target order/duplicates
only where semantics allow it. Table/algorithm parameters affecting resumable
work units have their own compatibility fingerprint bound to each lease.

Store schema version, algorithm semantic version, table checksum, and software
version. Hardware, thread count, GPU model, and launch geometry are execution
metadata, not logical job identity: compatible CPU/HIP/CUDA workers should be
able to resume the same committed interval. If a configuration cannot preserve
coverage semantics, reject it or create an explicit new job/migration. Never
silently reuse progress against different target files or corrupted table caches.

### Commit ordering and replay

```text
claim exact interval and generation
    -> submit bounded batch
    -> await successful device completion
    -> retrieve every candidate; reject overflow
    -> exact CPU verification
    -> commit results + coverage + frontier + event
    -> acknowledge progress and schedule more work
```

Keep completion per submitted batch. With multiple streams or target groups,
only advance a contiguous frontier when every prerequisite batch/target in that
prefix is verified. A later completed batch cannot jump over unfinished work.
Bound outstanding batches so replay and result buffers remain bounded; optionally
record disjoint completed intervals where that reduces replay safely.

On process termination, device failure, or lost power, replay everything after
the last acknowledged durable boundary. Results are idempotent, so replay cannot
create contradictory completion. The guarantee is at-least-once computation with
exact durable coverage, not exactly-once GPU execution. Expected replay is bounded
by the checkpoint interval plus outstanding work and commit latency; measure this
under load. Corrupt or incompatible state must fail visibly, never imply done.

### Pause controls

- `SIGINT`/`SIGTERM`: request a graceful checkpoint and exit. Signal handlers
  only set a flag or wake a control loop; they never perform database or GPU work.
- An explicit pause command (optionally `SIGUSR1` on Linux) stops new submissions,
  drains bounded in-flight batches, verifies results, commits, then stays idle.
  Resume revalidates state and obtains valid leases before launching work.
- A second interrupt may force exit; explain that uncommitted work will replay.
  Hard GPU/driver failures follow the same durable-boundary rule.
- Default distributed pause releases the uncommitted lease suffix after saving
  progress. Other workers may continue it; resuming a machine does not restore
  ownership of work already reassigned. Job-wide pause is a separate coordinator
  operation preventing all new claims.
- Publish pause latency and distinguish stopping launches from being durably
  paused. SIGSTOP alone is not a checkpoint and may cause lease expiry.

Standalone portable snapshot exports use temporary-file write, checksum, file
sync, atomic rename, and parent-directory sync. Back up an active SQLite database
through its supported backup facilities; do not copy only the main file while
its WAL is live. Keep pre-migration backups and a recovery/inspection command.

## 8. Multiple devices and computers

### Online coordination

Use a dedicated coordinator process behind Apache on the home server. The
proposed origin is `https://dbkeyprogress.rumahsimanis.bagus.my.id`, using standard
HTTPS TCP 443. Apache requires mutual TLS with individually approved client
certificates, then proxies requests over a protected Unix socket. The coordinator
checks registered credentials and project roles on every read/write. SQLite and
all private keys remain outside Git. See the
[server plan](COORDINATOR_SERVER_PLAN.md) for enrollment, revocation, proxy trust,
project isolation, home-network setup, and recovery.

Workers do not mount or directly write the coordinator's database. Optional local
execution uses the same scheduler library with standalone state. A larger SQL
service can replace the store if measured traffic exceeds the single-writer
design; separate database files per project are not required initially.

Proposed operations: register capabilities, claim (sequential/random/exact block),
heartbeat, report progress/results, complete, release, pause/resume job, inspect
coverage, and export/import assignments. Every operation is project-scoped; a
worker may update only its own current lease. Validate manifests, bound payloads,
and use exact hex integers. Reading unfinished blocks is only a preview; claiming
work is atomic. Coordination initially assumes cooperative workers; authentication
does not prove that a client searched a range.

Claim and completion transitions use transactions and compare the lease's worker,
generation, job digest, and range. Use coordinator time for deadlines. As initial
operational defaults, heartbeat every 15 seconds with a 90-second renewable lease;
heartbeats run independently of GPU execution and checkpoint I/O. Tune from
measured network/pause latency.

When a lease expires, preserve committed coverage and reclaim only its uncovered
suffix. Reissue it with a higher fencing generation. Stale workers may submit
candidate results for independent verification but cannot extend coverage or
finish the new lease. Idempotency keys make lost responses and duplicate reports
safe. Test coordinator restart, clock discontinuity, network partitions, and two
workers claiming the same block. A new coordinator epoch fences ambiguous leases
after recovery; only one active authoritative coordinator is supported initially.

Workers retain unacknowledged results/progress in a local durable outbox and retry
until acknowledged. A lost connection does not authorize new global allocations.
Stop claiming and eventually drain/pause on lease-renewal failure. Reconcile the
outbox against current lease generations before advancing global coverage.

For multiple GPUs, use independent device contexts/queues with a host worker per
selected logical device and bounded aggregate memory. Select by stable identity
where available and print the visible-index mapping. Respect visibility masks.
Avoid mandatory peer-to-peer memory or collectives for independent search ranges.
Benchmark per partition and per physical package; use dynamic leases to balance
different GPUs and CPU workers.

### Offline distribution

Add export/import after online correctness. Export exact disjoint ranges and job
metadata into checksummed assignment manifests. The coordinator durably reserves
exported ranges; they do not automatically become available on lease timeout.
Import merges exact compatible coverage and deduplicated results transactionally.
Lost assignments require explicit revocation before reassignment, with generation
checks preventing stale completion reports from overriding newer work.

Disconnected machines independently choosing random blocks cannot guarantee
non-overlap. Give them distinct reserved manifests, or clearly accept duplicate
computation. Offline reservations must be visible in status so users understand
why those ranges are unavailable to online workers.

### Proposed CLI sketch

These names illustrate the intended interface; finalize them with CLI compatibility
tests. All new range endpoints below are explicitly end-exclusive.

```sh
# On the coordinator server; Apache exposes the authenticated HTTPS endpoint.
keyhunt coordinator --state /var/lib/keyhunt-coordinator/progress.sqlite \
  --listen-unix /run/keyhunt-coordinator/api.sock

# On a remote worker after enrollment and a project-role grant.
keyhunt devices --backend hip
keyhunt worker --coordinator https://dbkeyprogress.rumahsimanis.bagus.my.id \
  --project PROJECT_UUID --job JOB_DIGEST \
  --tls-cert "$HOME/.config/keyhunt/worker.crt" \
  --tls-key "$HOME/.config/keyhunt/worker.key" \
  --backend hip --devices 0 --select random --checkpoint-interval 10s

# Optional standalone/offline operation, with state outside the repository.
keyhunt job create --mode bsgs --targets targets.txt \
  --range-start 0x1000 --range-end-exclusive 0x2000 --block-bits 8 \
  --state "$HOME/.local/state/keyhunt/demo.sqlite"
keyhunt run --state "$HOME/.local/state/keyhunt/demo.sqlite" \
  --backend hip --block 0x3 --once
keyhunt status --state "$HOME/.local/state/keyhunt/demo.sqlite"
keyhunt pause --state "$HOME/.local/state/keyhunt/demo.sqlite"
keyhunt resume --state "$HOME/.local/state/keyhunt/demo.sqlite" \
  --backend hip --devices 0
```

The server plan specifies credential handling and authorization; active-process
controls and command names still require implementation tests. Legacy search flags
remain supported by a compatibility parser.

## 9. README and supporting documentation

Adapt the requested [Best-README-Template][readme-template] to this project, with
these sections: project identity and status, table of contents, About the Project,
Built With, Getting Started (prerequisites/build), Usage, Roadmap, Contributing,
License, Contact, and Acknowledgments. Add a concise tested-backend/mode matrix
near the top. Use repository issue/discussion links for contact; do not invent
maintainer details, badges, release numbers, or benchmark claims.

Keep the README focused on installation and a reproducible small example.
Show CPU commands immediately; label HIP/CUDA examples planned until their gates
pass. Later add device selection, checkpoint recovery, random/sequential block
selection, and worker setup. Record exact validated SDK/compiler combinations and
the MI300X partition caveat. Preserve MIT and dependency notices, credit the
original keyhunt authors and verified reference contributions, and acknowledge the
README template without adopting its license as the project's license.

Move lengthy current mode examples and explanation into `docs/USAGE.md` and
`docs/MODES.md`; keep or redirect existing anchors and `BSGSD.md`. Add build,
architecture, checkpoint/distribution, and performance guides as the corresponding
features land. Performance tables link to reproducible benchmark artifacts and
state physical/logical GPU counts. Keep the roadmap's planned work distinct from
released capabilities. Check local links and run executable examples in CI.

## 10. Commit-sized implementation sequence

One logical change per commit. Do not mix file moves, algorithm changes, and
performance tuning. Every implementation commit includes the directly relevant
tests and documentation; split further if a row is too large to review. The
current planning commit precedes C01 and changes no program behavior.

| ID | Suggested commit | Dependencies | Acceptance gate |
| --- | --- | --- | --- |
| C01 | `test: capture CPU behavior and hardware baseline` | Plan | Existing modes characterized on small deterministic inputs; endpoint semantics, build environment, and benchmark methodology recorded |
| C02 | `build: organize sources and introduce target-based CMake` | C01 | Mechanical moves preserve outputs; CPU/legacy/bsgsd builds and fixtures work; no GPU SDK required |
| C03 | `docs: redesign README and extract usage guides` | C02 | Template adapted, current features accurately labeled, links and CPU examples verified |
| C04 | `refactor: extract configuration targets and result verification` | C02 | CLI regression checks pass; core interfaces do not depend on GPU headers |
| C05 | `feat: add exact range and work-unit planning` | C04 | Exhaustive small-domain and randomized large-integer tests prove no gaps, overflow, or unintended overlap |
| C06 | `test: add independent arithmetic and search vectors` | C04 | Oracle agreement, infinity/alias/carry cases, negative cases, boundary matches |
| C07 | `feat: add HIP device discovery and execution backend` | C05, C06 | gfx942 build and small launch pass on this host; device/partition/memory identity and failures reported correctly |
| C08 | `feat: implement portable GPU field and point arithmetic` | C07 | GPU/CPU/oracle differential tests pass, including in-place operations and partial batches |
| C09 | `feat: execute bounded xpoint searches on HIP` | C08 | Nonzero and >64-bit ranges, no-match cases, exact coverage, candidate overflow/replay validated |
| C10 | `feat: add versioned GPU BSGS table preparation` | C08 | Table format, collision handling, checksums, memory budgeting, and CPU/GPU filter agreement validated |
| C11 | `feat: implement complete HIP BSGS range search` | C09, C10 | Tiny exhaustive and seeded large-offset BSGS cases match CPU/oracle, including tile ends and all targets |
| C12 | `feat: persist sparse coverage and transactional leases` | C05 | External state directories, project-scoped schema, lazy allocation, interval merging, selection policies, wide IDs, and concurrent claims pass |
| C13 | `feat: checkpoint verified progress and replay incomplete work` | C09, C11, C12 | Faults at every commit boundary preserve matches and coverage; mismatched/corrupt jobs rejected |
| C14 | `feat: add graceful pause resume and state inspection` | C13 | Signals/control path, bounded drain, resume with changed device count, backup and migration checks pass |
| C15 | `feat: coordinate authenticated project-scoped workers` | C12–C14 | Separate S02–S06 commits in the server plan: Apache mTLS, project roles, revocation, fenced leases, retries, outbox, two-host operation, and restore pass |
| C16 | `perf: add reproducible GPU profiling and benchmark harness` | C11, C14 | Comparable timing/coverage metrics, raw repeated samples, hardware and durability metadata recorded |
| C17 | `perf: tune HIP batching arithmetic and memory layout` | C16 | One measured optimization per commit; correctness, pause latency, and throughput gates preserved |
| C18 | `feat: add native CUDA backend over shared kernels` | C11, C13 | NVIDIA compile and real-hardware parity/recovery tests pass; backend matrix updated |
| C19 | `perf: add validated gfx942 arithmetic specializations` | C17 | One primitive per commit, portable fallback, disassembly and paired performance evidence |
| C20 | `feat: schedule and balance multiple GPU devices` | C14–C17 | No overlaps across workers; logical/physical scaling and memory-pressure tests pass |
| C21 | `docs: publish validated GPU build and operations guides` | C14, C16, C20 | HIP quickstarts match released behavior; CUDA/assembly sections follow C18/C19 validation; benchmark and recovery limitations documented |
| C22 | `feat: export and reconcile offline work assignments` | C15 | Reservations, duplicate imports, revocation generations, incompatible manifests, and exact union tested |
| C23 | `feat: extend GPU mode coverage` | C18, C20 | Separate commits for hash/address/other modes; each has algorithm and recovery parity before enablement |

C12 can be developed after C05 without waiting for HIP; its integration still
requires GPU completion contracts to pass. NVIDIA validation requires access to a
CUDA toolchain and NVIDIA device, which have not been established on this host.
An untested CUDA build must stay labeled experimental. GPU parity for the later
modes is an explicit follow-up phase, not an implied part of the first BSGS release.

## 11. Verification and release gates

| Area | Required checks |
| --- | --- |
| CPU compatibility | Existing modes, CLI errors, input parsing, compression variants, legacy/bsgsd build compatibility, sanitizers on new host code |
| Arithmetic | Independent known vectors; randomized differential field/point/scalar tests; carries, zero, infinity, aliasing, final reduction |
| GPU coverage | First/last/batch-boundary matches, no matches, nonzero high-bit starts, partial workgroups, >32-bit giant indices, target groups |
| BSGS | CPU/GPU filter agreement, exact collision lists, both point signs, tails, memory limits, corrupted caches, out-of-range candidates |
| Candidate handling | Deliberately tiny buffers, overflow replay, slow verification, duplicate candidates, device errors; no early completion |
| Scheduling | Exhaustive small partitions; variable lease spans; manual busy/done blocks; random selection near exhaustion; high-bit IDs and sparse state |
| Recovery | Kill before/after GPU completion, result commit, coverage commit, and acknowledgment; disk-full/failed sync; resume and deduplication |
| Distributed | Simultaneous claims, lease expiry, stale generations, lost replies, heartbeat failure, coordinator epoch recovery, offline reservations |
| Server access | Required client certificates, registered credentials, cross-project denial, lease ownership, forged proxy headers, revocation on live connections, private socket boundary, credential-safe restore |
| Performance | Repeated equivalent workloads, one partition/package/multiple packages, storage overhead, allocation headroom, pause and replay bounds |

CPU CI runs without GPU SDKs. Backend compile checks and real-hardware integration
checks are separate; a compile-only green result cannot certify a GPU release.
GPU jobs run small deterministic workloads first, then opt-in benchmarks. Keep
performance noise out of correctness gates and retain failures for inspection.

Before the first HIP release, require C01–C14 and a reproducible measured baseline.
Before distributed use, require C15 fault-injection and server access-control
coverage. Before advertising NVIDIA support, require C18 hardware validation.
Before enabling assembly by
default, require C19 correctness and performance evidence on the supported stack.

## 12. Decisions to revisit with implementation evidence

- Select BSGS `m`, filter density, limb representation, and batch size from
  measured per-agent memory and cost, not the whole-card specification.
- Determine whether the installed CMake/ROCm combination needs a project-local
  newer CMake; do not change the system toolchain just for this planning step.
- Confirm physical package mapping for all 64 logical agents before multi-GPU
  performance claims. Do not change machine partition or power settings as part
  of normal application startup.
- Finalize current CLI endpoint/stride/random semantics during C01. Exact
  journaled coverage may require a new selection mode instead of relabeling the
  legacy random loop as exhaustive.
- Benchmark sparse-index fragmentation, random rank selection, and transaction
  throughput before adding bitmap pages or moving beyond SQLite.
- Record the first supported NVIDIA architecture/toolkit when hardware is
  available; retain the shared-kernel and native-runtime design in the meantime.

## References

Local observations refer to the commits identified at the top of this document.
External documentation was consulted on 2026-09-29; pin toolchain-specific
references again when implementation versions are selected.

- [Best-README-Template][readme-template]
- [ROCm Core SDK 10.0 release notes][rocm-release]
- [AMD GPU specifications][amd-specs]
- [AMD compute and memory partition profiling][partitions]
- [HIP porting guide][hip-porting]
- [CMake 3.22 HIP architecture selection][cmake]
- [LLVM AMDGPU usage and ABI][llvm]
- [NVIDIA inline PTX assembly][ptx]
- [SQLite WAL][sqlite-wal] and [synchronization settings][sqlite-sync]

[readme-template]: https://github.com/othneildrew/Best-README-Template
[rocm-release]: https://rocm.docs.amd.com/en/latest/about/release-notes.html
[amd-specs]: https://rocm.docs.amd.com/en/docs-10.0.0/reference/gpu-specs.html
[partitions]: https://rocm.docs.amd.com/projects/rocprofiler-compute/en/develop/conceptual/cdna/compute-memory-partition.html
[hip-porting]: https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_porting_guide.html
[cmake]: https://cmake.org/cmake/help/v3.22/prop_tgt/HIP_ARCHITECTURES.html
[llvm]: https://llvm.org/docs/AMDGPUUsage.html
[ptx]: https://docs.nvidia.com/cuda/inline-ptx-assembly/
[sqlite-wal]: https://www.sqlite.org/wal.html
[sqlite-sync]: https://sqlite.org/pragma.html#pragma_synchronous
