# C21: validated GPU build and operations guides

C21 is complete for the existing HIP/CUDA validation scope. The current entry
points are the [build matrix](BUILD.md#validated-gpu-builds),
[finite GPU quickstart](GPU_QUICKSTART.md), [mode matrix](MODES.md), and
[operations guide](OPERATIONS.md). README links these alongside the retained CPU
examples and historical anchors. C22 offline assignment files and C23 further GPU
modes remain planned.

## Findings and decisions

Several live guides still described coordination, CUDA builds or concurrent
checkpoint owners as future work. They now match the C18–C20 implementation while
retaining historical acceptance sections and raw evidence. The old proposed CLI
sketch has been replaced by actual command names and operational links. The
checkpoint guide distinguishes the original v2 migration from the current v6
schema; the signal guide no longer promises immediate termination of a process
stuck inside a driver.

The build guide records exact tested SDK/compiler combinations, separate native
builds, optional worker dependencies and installed executable names. C19 remains
opt-in, and rejected AMD assembly/CUDA tuning experiments remain documented.
MI300X partition counts, unvalidated MIG/QPX search, and localhost-only coordinator
validation are explicit. The performance guide separates kernel timings,
process costs, finite fleet rates and twelve-hour calibration predictions.

The quickstart supplies its own public generator fixtures and private state. It
uses hexadecimal half-open ranges, CPU-built versioned BSGS tables, both claim
policies, durable results and completed-grant replay. The commented
[example harness](../tests/integration/gpu_examples.py) reads marked Bash blocks
straight from the document. CPU CI executes the preparation/assignment blocks;
GPU execution requires an explicit hardware invocation and cannot be inferred
from a passing CPU job. The existing README CPU example gate remains in CI.

During harness development the first HIP check mistakenly counted both BSGS
batch output and its all-target tile receipt as coverage. The validator now
collects matches from batches and credits coverage only at tile receipts. The
search commands themselves succeeded; no production search change was needed.
Final HIP and CUDA checks pass with that distinction enforced.

## Validation

[The acceptance manifest](baselines/C21_VALIDATION.json) records document/harness,
binary and artifact hashes. [Raw quickstart results](baselines/C21_EXAMPLES.json)
retain commands' JSON/NDJSON output; [raw localhost operations](baselines/C21_OPERATIONS.json)
retain launch/sync/supervisor/status/audit commands and responses. The
[small log archive](baselines/C21_LOGS.tar.gz) includes the old CPU example report,
link/YAML checks and the one-off operations reproduction script. No credential
contents, private keys, journal files or test CA are included.

| Check | Result |
| --- | --- |
| README CPU address and BSGS examples | Both pass, including the legacy BSGS success exit status 1 |
| New quickstart, CPU build | Table build/inspect, canonical xpoint/BSGS jobs, sequential/random claims and journal audit pass; no GPU execution claimed |
| New quickstart, MI300X ordinal 0 | Discovery, diagnostic launch, table validation, both exact volatile searches, durable searches and zero-batch completed retries pass |
| New quickstart, H200 ordinal 7 | Same checks pass with all eight GPUs visible |
| Localhost operations, HIP and CUDA | Both pass readiness, manual sync, persistent execution, local-complete/unacknowledged status, explicit upload, server acknowledgment and journal audit |
| Workflow and documentation | YAML parses; all local links, anchors and fences pass in 48 Markdown files; patch whitespace check passes |

Each xpoint run covers exactly `[1, 0x101)` (256 scalars); each BSGS run covers
`[1, 0x10001)` (65,536 scalars), with `m=257`. Each yields exactly one scalar-1
match. The harness checks the volatile receipt union, finished durable block union,
empty remaining coverage, stored match set, and zero new computation on retry.
Both localhost tests confirm that execution leaves an outbox until manual sync
and does not alter the last server acknowledgment during execution.

The hardware binaries match their C20 acceptance hashes. Production sources and
runtime scripts were unchanged. HIP uses the existing C19-enabled
`build/hip-release`; H200 uses `build/cuda-c20`, the previously validated
`cuda-h200` preset with optional workers. The compiler/dependency stacks and broad
arithmetic, fault, pause, memory and fleet gates remain in
[C20 HIP](MULTI_GPU.md) and [C20 CUDA](C20_CUDA_VALIDATION.md). C21 adds command and
document checks; it does not rerun those full suites or claim a new tuning result.

## Reproduction and limits

Run `python3 tools/check_docs.py` and the three example-harness commands in
[the quickstart](GPU_QUICKSTART.md#check-the-documented-commands). Run
`tests/integration/readme_examples.py --binary BUILD/keyhunt --report REPORT`
with Python for the retained CPU examples. Hardware examples run sequentially on
one selected device; their wall times are not benchmarks. The operations script
in the archive accepts `--repo`, `--build`, `--apache-root`, `--backend`, optional
`--device`, and `--report`; it creates disposable localhost state and removes its
own certificates/journals after stopping the launcher.

The CI workflow change was validated locally, including its new CPU command; no
new GitHub-hosted workflow run or public deployment is claimed. Cross-host
coordinator traffic, public ingress/ACME, real driver-hang recovery, additional
GPU modes and portable offline assignment files retain their documented limits.
