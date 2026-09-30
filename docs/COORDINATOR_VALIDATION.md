# C15 validation and findings

## Sanitizer recovery fixture

The first focused ASAN/UBSAN run passed 16 of 17 gates. LeakSanitizer traced the
failure to the recovery test's nested JSON initializer-list wrappers when its
embedded repository request threw for an expired/fenced assignment. The disk-full
transaction assertions themselves passed; no SQLite resource leak was reported.

The fixture now completes the throwing request before constructing a successful
transport envelope. This matches the real HTTPS boundary, which returns an error
without constructing a successful response. No production error handling was
suppressed and no sanitizer exclusion was added. The recovery gate then passed,
along with the new local-launch/budget gates and twenty additional core gates.

## Coordinator transaction cost

The 32-machine burst initially measured 130.08 ms median / 130.43 ms p95 for
progress plus renewal. Inspection found a new secp256k1 verifier constructed for
each updated grant, including lease-only and no-match updates. This unnecessarily
held the write transaction while initializing curve state.

The implementation now resolves a target binding once per job only when a page
contains matches, and creates one verifier per machine sync only when needed.
Every submitted match still receives the same CPU target/scalar verification.
After this change, the identical 32-machine/64-device/128-grant fixture measured
2.88 ms median / 3.19 ms p95 (5.00 ms maximum). Claim latency stayed near 2 ms.
These are local CPU repository timings, not GPU throughput or WAN capacity.
Random block selection changes touched database pages, so database/WAL sizes are
reported as observations rather than attributed to this optimization.
