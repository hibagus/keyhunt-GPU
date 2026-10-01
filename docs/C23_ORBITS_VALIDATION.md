# C23 related-key orbit acceptance

Explicit `--endomorphism orbit` is implemented for native xpoint, Bitcoin
HASH160/P2PKH, Ethereum and vanity, with HIP/CUDA search parity, durable recovery
and online/offline worker parity. C23 overall remains partial: additional search
mappings need their own coverage contracts and acceptance gates.

## Coverage and implementation decisions

A seed k expands to `k`, `-k`, `lambda*k`, `-lambda*k`, `lambda²*k`, `-lambda²*k`
modulo n. Input bounds and positive stride define N seeds. Candidate coordinates
are variant-major: `j=v*N+i`, with variants 0..5 and seed positions 1..N. Reverse
changes seed order within each variant. Derived private scalars can lie outside
the seed range; completion certifies `[1,6*N+1)`, not a contiguous scalar range.
Overlapping seed orbits retain separate candidate/target observations.

The current journal domain requires `6*N+1 <= n`. Jobs that exceed this limit
are rejected with a request to split the seed range. Configuration versions 4/5
bind forward/reverse orbit coverage; old versions 1/2/3, default `none`, schema 7
and receipt encoding are preserved. Public results expose expanded index, seed,
variant and actual scalar under `scalar-orbit-index-v1`.

Batches stop at variant boundaries even when blocks or grants cross them.
Direct/GLV derives the seed point and transforms it in the field; stepped uses
the derived base point and one of six cached signed point-step tables. CPU
verification independently reconstructs the actual private scalar modulo n.
A single variant is injective, preserving xpoint's compact per-batch output bound.
Prepared targets and caches survive grant handoff. Whole-attempt overflow
replays cannot certify partial coverage.

Workers advertise `scalar-orbit-v1`. Incompatible requests fail before allocation,
renewal, update or cached reply; older capability sets retain their original jobs.
BSGS and minikeys reject this option. Kernel choice remains execution-only and
can change on restart. See [the contract and public example](C23_ORBITS.md).

## Recorded validation

The native stacks used eight visible SPX/NPS1 MI300X devices and eight H200s.
All three scalar kernels are covered on both backends; stepped spot checks run
on each visible ordinal. This is not an exhaustive kernel-by-ordinal product,
fleet scaling measurement or additional partition certification.

| Gate | Observed coverage |
| --- | --- |
| CPU release | All 107 distinct gates have passing results after one fixture correction; the new documented example also passed |
| Integer mapping | 7,048 independent cases, exact inverse coordinates, maximum job bound and exhaustive small block/work/batch partitions |
| Point arithmetic | 2,521 cases per portable/HIP/CUDA implementation, including six variants, aliasing, high bits, near-order seeds and invalid inputs |
| Native CLI | 164 cases and 32 invalid mapping rejections per backend; four families, three kernels, both orders, all eight ordinals |
| Search boundaries | Unit/wide strides, short tails, misses, overlapping seed orbits, overlapping vanity prefixes, overflow replay and maximum-batch tails |
| Checkpoints | 56 cases per backend, including eight killed-process restarts; all six variants and repeated observations survive exact replay |
| Pause/visibility | Four family cases per backend; GLV → direct → stepped, repeated socket/signal pause, changed visibility, backup and restore quarantine |
| HTTPS/file workers | 16 cases per backend; two grants cut inside a variant, one executor setup, exact local/server results, retry and disconnected execution |
| Regression | Existing executor/failure gates, HTTPS/offline workers, old capability compatibility and CUDA primary-context isolation |
| Sanitizers | Five focused ASan/UBSan host, portable point, storage and coordinator gates |
| Documentation | CPU preparation plus native public orbit example and completed-grant replay on HIP and CUDA |

The [manifest](baselines/C23_ORBITS_VALIDATION.json) records source revisions,
binary hashes, build/compiler settings and artifact checksums.
[Algorithms](baselines/C23_ORBITS_ALGORITHMS.json),
[recovery](baselines/C23_ORBITS_RECOVERY.json),
[workers](baselines/C23_ORBITS_WORKERS.json),
[examples](baselines/C23_ORBITS_EXAMPLES.json) and
[raw logs](baselines/C23_ORBITS_LOGS.tar.gz) retain the detailed observations.
No enrollment credentials or journal databases are archived.

## Findings and limits

The initial CPU run passed 106/107 tests. An older stride denial fixture removed
two entries from the new nine-capability list, leaving a valid seven-capability
worker. Truncating the simulated request to six restored the intended rejection;
the focused rerun passed without changing production capability rules.

The first HIP example harness treated the summary's hexadecimal match count as
relation records. Selecting batch records fixed the harness. The program already
returned the expected scalar 1 and n−1 at indices 3 and 6; the corrected harness
also checks all 18 candidate boundaries. Initial failures and successful reruns
are retained. Existing explicit-constructor warnings remain outside this slice.

CUDA search and integration used separate immutable binaries and overlapped.
The example used a separate documentation checkout with the integration binary.
The second CUDA checkout was then synchronized and passed five focused mapping,
recovery and context-isolation checks. Final documentation builds preserved
production binary hashes. The sanitizer result covers five focused host tests,
not the complete application or GPU device code.

This slice establishes correctness and recovery. It makes no throughput claim,
changes no kernel default, and adds no calibration, public deployment, BSGS or
minikey expansion. Orbit counts describe candidate observations; they are not a
claim of six times as many distinct private scalars when seed orbits overlap.

## Reproduction

Build using [BUILD.md](BUILD.md) with coordinator/HTTPS enabled for transport
gates. Set the existing [pinned Keccak oracle environment](C23_ETHEREUM_VALIDATION.md#reproduction-and-limits),
then run `ctest --test-dir BUILD --output-on-failure -R orbit`. Native recovery
uses a writable private journal parent outside Git checkouts (`TMPDIR=/var/tmp`
on the H200 host). The transport gates require the existing Apache test setup.
Run the standard scalar executor/failure tests, `coordinator_https_worker`,
`coordinator_offline_cli`, and CUDA's `coordinator_cuda_contexts` for the recorded
regression coverage. The [public example](C23_ORBITS.md#executable-public-example)
is executable through `tests/integration/orbit_examples.py`.
