# C06 arithmetic and GPU readiness audit

Audited revision: `158f5c7` on 2026-09-29, covering C04–C06. C07 implementation
started during this review; these conclusions apply to a frozen C06 checkout.

**C06 passes its stated arithmetic and host-planning gates, but the real CPU
search loops still silently miss valid targets.** The independent release run
passed all 11 suites, and the focused ASan/UBSan run passed all nine suites. An
additional 1,152 random point checks also passed. Separate searches through the
actual CLI reproduced the coverage failures below. They also occur in C03 and
are inherited defects, not regressions attributed to the C06 changes.

C07 discovery and bounded launch work can proceed. The first GPU search must
handle exceptional points explicitly and must not use the legacy loops as an
exhaustive coverage oracle. This follows the existing design's separation of
arithmetic, planning, and search execution.

## A10 Point stepping still misses valid targets

Priority: **P1 before reuse in C08–C11**. This supplies end-to-end evidence for
the first audit's A02 batch-inversion warning.

The audit generates public points through the pinned libsecp256k1 executable and
checks them against the independent Python affine model. Every search uses a fresh
temporary directory, one CPU thread, synthetic targets, and a timeout. Hexadecimal
ranges below are half-open expected intervals; all listed targets lie inside them.

| Mode and input | Expected matches | Observed C06 matches | Missing targets |
| --- | ---: | ---: | --- |
| xpoint, `-r 200:600 -n 1024` | 6 | 1 | `200`, `201`, `3ff`, `401`, `5ff`; only `400` is found |
| xpoint, `[n-19,n)`, decimal 19 | 3 | 0 | `n-19`, `n-2`, `n-1` |
| xpoint, `[n-1024,n)`, decimal 1024 | 4 | 0 | `n-1024`, `n-19`, `n-2`, `n-1` |
| xpoint, `[n-1025,n)`, decimal 1025 | 3 | 2 | `n-1` |
| BSGS, `-r 100000:300000 -n 1048576` | 12 | 9 | Scalars `100400`, `100c00`, `200400` |

Every C06 search above exits **0**. The first xpoint job has exactly one aligned
1,024-scalar batch, so its five missed targets cannot be explained by the already
documented final-tail overrun. The BSGS misses correspond to offsets `0x400`,
`0xc00`, and `0x100400` from the start. Its other nine supplied targets include the
start scalar, confirming that C06 fixed that previously documented start miss.

Source anchors below refer to the audited revision:

- [`src/app/keyhunt.cpp`](../../src/app/keyhunt.cpp):2454–2466 computes the batch
  center and denominators and calls `grp->ModInv()` unconditionally. For the
  `[0x200,0x600)` case, the center is `0x400*G`, equal to `_2Gn`. The denominator
  for the next center is therefore zero, even though the requested range is valid.
- [`IntGroup.cpp`](../../src/crypto/secp256k1/IntGroup.cpp):36–56 forms one product
  of all denominators. Its nonzero-input requirement is violated by the caller.
  A separate probe of `ModInv([2,0,3])` returns `[0,0,0]`; one exceptional input
  invalidates the other results. The unchanged center point explains why `400`
  alone survives the direct-search example.
- The BSGS loop at `src/app/keyhunt.cpp:3728–3746` likewise assumes that its start
  point and all denominators are usable by its affine formulas. The tested offsets
  make that start point coincide with a precomputed point or become infinity.
- Near `n`, the direct loop's artificial center can be outside the private-scalar
  domain. `ComputePublicKey` at
  [`SECP256K1.cpp`](../../src/crypto/secp256k1/SECP256K1.cpp):61–68 correctly returns
  the infinity sentinel for such an input, but the loop then continues using
  affine formulas. In the aligned near-order case, a denominator instead vanishes
  because equal X coordinates represent opposite points.

Keep the strict private-scalar verifier. Fix the stepping algorithm rather than
weakening that check: exclude zero denominators from the shared inverse product,
carry an exception mask, and handle doubling, inverse addition, and infinity
correctly. An artificial seed must use a defined group operation or a valid seed
inside the batch; an invalid-private-key sentinel is not an affine point.
Do not reduce candidate scalars modulo `n` and credit them to another interval.

For performance, benchmark a regular path with masked denominators plus explicit
exception processing. Returning early from a lane before collective inversion is
not safe. Padded lanes should contribute the multiplicative identity, then emit
no candidates. The correct treatment must preserve the unaffected lanes instead
of replaying every normal batch with individual inversions.

Acceptance: reproduce every case above with the actual new executor, verify every
requested target, and instrument every visited candidate in small no-match jobs.
Include zero at each position of an inversion group, multiple zeros, infinity,
equal/opposite points, partial groups, and counts around wave/workgroup boundaries.
If the CPU loops remain legacy-only, record these specific misses as known defects
and continue to exclude their successful exits from coverage accounting.

## Why the current green suites do not catch A10

The separation is accurately documented in
[ARITHMETIC_ORACLE.md](../ARITHMETIC_ORACLE.md#bounded-search-corpus-and-final-gate),
but it needs to remain visible when approving the next milestones:

- [`field_oracle.py`](../../tests/oracle/field_oracle.py):56–61 tests group sizes up
  to 64 with nonzero inputs only. This proves the routine's supported precondition;
  it does not show that search callers satisfy it.
- [`bounded_search_probe.cpp`](../../tests/oracle/bounded_search_probe.cpp):59–67
  derives each scalar independently through the verifier. It tests C05 planning
  and target relations, bypassing grouped point stepping, filters, and tables.
- [`search_oracle.py`](../../tests/oracle/search_oracle.py):55–84 implements a tiny
  BSGS mathematical reference in Python. Its 560 passing cases do not execute
  the production BSGS loop or a GPU kernel.

Retain these tests and their scopes. Add actual executor comparisons when C09 and
C11 arrive, using the same fixed fixtures plus the cases discovered here. A pass
for independently deriving every scalar cannot certify an optimized recurrence
that might omit or corrupt candidates.

## A11 Broaden the reusable point corpus

Priority: **P2 for C06 test maintenance and C08 reuse**. No new arithmetic failure
was found by the additional checks.

[`point_oracle.py`](../../tests/oracle/point_oracle.py):23–27 creates 96 random
scalars, but selects `scalars[9:25]` for its point-operation pool. That slice
contains 15 boundary scalars and only the first random scalar. Random pair
selection subsequently operates on this small fixed pool. The remaining random
scalars do exercise public-key derivation, but not arbitrary pairs of full-width
random points. Projective rescaling uses only `1`, `7`, and `p-1`.

The audit added 128 distinct seeded random points, 256 random pairs, and full-width
nonzero homogeneous scales. It checked general/mixed/direct addition, both doubling
paths, and repeated reduction: **1,152 operation cases passed**, including the
probe's returned-result assignment variants. Keep this expanded population in the
reusable corpus before relying on it for device arithmetic changes.

Also preserve the coordinate distinction: C06 CPU `Point` uses homogeneous
`(X/Z,Y/Z)`. The external CUDA prototype uses Jacobian `(X/Z²,Y/Z³)`. Compare their
affine outputs through representation-specific adapters; copying raw X/Y/Z or the
CPU rescaling adapter would not test the same group element. CPU returned-value
assignment tests also do not replace device output-pointer alias tests.

## GPU integration and performance actions

These are concrete next-stage requirements, not measured GPU improvements or
defects in the completed host planner.

| Gate | Action | Reason and acceptance evidence |
| --- | --- | --- |
| C07–C09 | Introduce a compact device batch descriptor; keep `WorkUnit`, checked host integers, and execution identity on the host | Derive seed/count once per batch. Check device grid products before multiplication and test indices above `2^32`, partial groups, and overflow. Avoid per-candidate host planning or transfers. |
| C08 | Compare small per-thread inversion tiles with wave/workgroup inversion using A10's exceptional inputs | Measure VGPR/register use, scratch, LDS/shared memory, and time per useful point together. A fast path that loses exceptional candidates cannot pass. |
| C09 | Add an exact, owned 32-byte xpoint target set and a defined host/device representation | `CpuTargetTable` still stores 20-byte prefixes by design. Its cache cannot provide the missing suffix. Retain collision candidates for full-width CPU verification; test two targets sharing the first 20 bytes. |
| C09–C11 | Compare a derived candidate against all relevant targets without repeating scalar multiplication for each duplicate target | Derive once per unique scalar, then compare exact target bytes. Keep target identity and deduplication consistent with job semantics. Benchmark dense-match cases as well as normal no-match scans. |
| C07–C08 | Start event timing and compiler resource capture now, as recommended in A06 | C06 changed arithmetic costs. Test durations and the short C01 process benchmark are not a valid primitive or GPU throughput baseline. Preserve canonical reduction and exceptional-point correctness while tuning. |

Full GPU output verification should occur only for emitted candidates, with
bounded host queues. The C06 test executor deliberately recomputes every scalar
for every target; it is an oracle harness, not a performance template.

## Status of the first audit

| Earlier finding | C06 disposition |
| --- | --- |
| A01 reference CUDA defects | Still excludes that external prototype from unvalidated reuse; C06 repaired separate CPU code |
| A02 CPU X-only public-key verification | Addressed by C04's exact X/Y verifier and independently checked here through the C06 suites |
| A02 zero-denominator handling | Still open; A10 now reproduces actual lost matches in direct and BSGS searches |
| A02 single-index fingerprint collision lookup | Still open in `bsgs_searchbinary`; C10/C11 must enumerate all exact candidates |
| A03–A09 performance and integration actions | Remain experiments/gates for the GPU implementation; no GPU throughput claim is made by this audit |

## Evidence and reproduction

[C06_AUDIT_VALIDATION.json](C06_AUDIT_VALIDATION.json) records the exact revisions,
commands, binary hashes, existing suite reports, and complete test summaries.
The release suites passed in 42.41 seconds; the nine focused sanitizer suites
passed in 26.27 seconds with leak detection and halt-on-UB enabled. The legacy CLI
baseline and loader suites remain excluded from that sanitizer run, as documented
by C06; the audit does not clear their existing alignment/leak findings.

[C06_AUDIT_PROBES.json](C06_AUDIT_PROBES.json) contains complete synthetic target
files, arguments, outputs, missing keys, prior-revision comparisons, and the
expanded random-point result. The frozen build/logs remain at
`/tmp/keyhunt-c06-audit-be1tvq4w` for local inspection.

Reproduce against a C06 Release build from the repository root:

```sh
python3 tools/audit_c06.py \
  --binary build/cpu-release/keyhunt \
  --oracle build/cpu-release/secp256k1_oracle \
  --arithmetic build/cpu-release/cpu_arithmetic_probe \
  --report /tmp/keyhunt-c06-audit.json
```

The command returns **1** while the search defects remain. An optional
`--previous-binary PATH` runs the same searches against an older build; this audit
used the earlier frozen C03 binary at `ad64ee8` and recorded its hash. It confirms
the inherited misses and the C06 BSGS start fix. Production sources, CMake, and
the other implementer's ongoing C07 work were not modified.
