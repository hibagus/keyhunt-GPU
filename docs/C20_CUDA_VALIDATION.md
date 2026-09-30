# C20 CUDA validation on H200

This follow-up runs the C20 concurrent worker on eight physical NVIDIA H200s.
The [acceptance manifest](baselines/C20_CUDA_VALIDATION.json) ties the source
revision, binaries, toolchain, test output and raw fleet reports together. The
[original HIP evidence](MULTI_GPU.md) remains a separate measured dataset.

## Environment and reproduction

The user-authorized SSH node reports eight H200s, each with 132 SMs and
143,771 MiB reported by `nvidia-smi`. MIG is disabled. The stack is Ubuntu
22.04.5, GCC 11.4, CMake 3.22.1, CUDA 13.3.73 and driver 610.57.04. All GPUs
were idle at the initial inspection. The host was not reserved; external load
is uncontrolled. Another CUDA audit appeared during correctness testing; the
fleet measurements waited until its GPU workload cleared. Two-second process
observations accompany the measured window. Validation changes no partition mode, clock, power limit,
system package or system service.

The remote checkout matched C20 before testing. A separate `build/cuda-c20`
directory enables CUDA, coordinator and HTTPS together. SQLite 3.51.3 comes
from the prior C18 local build. Apache, curl development files and nlohmann JSON
are extracted Ubuntu packages under `/tmp/keyhunt-c20-deps/root`; package hashes
are retained. The HTTPS fixture binds loopback with temporary test certificates.
No credential, worker journal or private signing key is included in the evidence.

```sh
cmake --preset cuda-h200 -B build/cuda-c20 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DKEYHUNT_ENABLE_COORDINATOR=ON -DKEYHUNT_ENABLE_HTTPS_WORKER=ON \
  -DKEYHUNT_TEST_APACHE_ROOT=/tmp/keyhunt-c20-deps/root \
  -Dnlohmann_json_DIR=/tmp/keyhunt-c20-deps/root/usr/lib/cmake/nlohmann_json \
  -DCURL_INCLUDE_DIR=/tmp/keyhunt-c20-deps/root/usr/include/x86_64-linux-gnu \
  -DCURL_LIBRARY=/tmp/keyhunt-c20-deps/root/usr/lib/x86_64-linux-gnu/libcurl.so \
  -DSQLite3_INCLUDE_DIR=/tmp/keyhunt-c18-deps/sqlite-autoconf-3510300 \
  -DSQLite3_LIBRARY=/tmp/keyhunt-c18-deps/libsqlite3.so
cmake --build build/cuda-c20 --parallel 12
TMPDIR=/var/tmp ctest --test-dir build/cuda-c20 --parallel 8 --output-on-failure
```

Dependency paths above describe this node; substitute installed development
packages or equivalent extracted roots elsewhere. `TMPDIR=/var/tmp` follows the
[C18 state-directory finding](C18_TEST_ENVIRONMENT.md): journals must remain
outside Git checkouts. GPU tests share the CTest device resource lock. The fleet
benchmarks start only after correctness tests and memory checks have finished.

## Correctness and context isolation

All **64 tests** pass: 23 CUDA hardware gates and 41 CPU gates in the CUDA /
coordinator build. No test is skipped or waived. The run includes 19,357 field
oracle cases, 1,278 point cases, arithmetic and table checks across all eight
H200s, 49 xpoint cases plus 29 rejections for each direct/stepped variant, and
95 BSGS cases plus 37 rejections for each automatic/group-1/group-8 variant.
Checkpoint kill/restart, changed-visibility pause/resume, injected errors, exact
candidate replay, the live HTTPS worker and two-device fleet all pass.

A separate CMake configure check retains the 47-test CPU/coordinator suite
without requiring CUDA. Its tests were not rerun for this CUDA-only test addition.

The new `coordinator_cuda_contexts` test links the production worker self-test
into a fresh process. Driver queries confirm all primary contexts start inactive,
then only the selected nondefault device becomes active. With eight GPUs visible,
ordinal 7 passes direct/stepped xpoint and group-1/group-8 BSGS while devices 0–6
stay inactive. The query itself does not retain or select a context. See the
[CUDA API contract and test explanation](CUDA_BACKEND.md#c20-worker-context-isolation).
This closes the NVIDIA hardware gap for audit A20. Single-visible-device systems
explicitly skip this isolation test.

Three NVIDIA Compute Sanitizer memcheck runs pass with zero memory errors or
leaks: xpoint executor ownership/tails/overflow/replay, BSGS ownership/replay for
all grouping policies, and the production worker self-test. The xpoint fixture
intentionally selects invalid ordinal `-1`, so only that run uses
`--report-api-errors no`; memory and leak checks remain enabled. BSGS and the
valid worker self-test retain API-error reporting. Raw commands/output are in the
[log archive](baselines/C20_CUDA_TEST_LOGS.tar.gz).

The context test also passes with `CUDA_VISIBLE_DEVICES=7,1`: only visible ordinal
1 becomes active and its UUID matches physical GPU 1. Final checks also cover `6,1` remapping and one visible GPU. An empty visibility
mask initially exposed a test-only defect: `cuInit` returns `CUDA_ERROR_NO_DEVICE`
before enumeration. The corrected gate returns skip 77 for both one and zero
visible devices and still passes on all eight. The original failure and focused
reruns are retained; production binary hashes are unchanged. This explicit skip-contract check is
separate from the all-visible 64-test run, which has no skips.

## Measured fleet behavior

All **48 runs** pass: eight warm-ups and forty measured runs, totaling **360 block completions**. Every selected GPU completes two blocks in every run. The [raw report](baselines/C20_CUDA_FLEET.json) retains each grant and timing.

| GPUs | Xpoint seconds, median [min–max] | Billion scalars/s | Relative to one GPU | BSGS seconds, median [min–max] | Billion effective scalars/s | Relative to one GPU |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 12.038 [12.014–12.073] | 0.714 | 1.00× | 4.479 [4.471–4.537] | 1.918 | 1.00× |
| 2 | 12.548 [12.511–12.615] | 1.369 | 1.92× | 4.972 [4.908–4.998] | 3.455 | 1.80× |
| 4 | 13.468 [13.395–13.554] | 2.551 | 3.58× | 5.948 [5.907–5.995] | 5.777 | 3.01× |
| 8 | 15.246 [15.208–15.268] | 4.507 | 6.32× | 7.793 [7.756–7.825] | 8.818 | 4.60× |

Each mode/device-count pair has one warm-up and five measured runs. Every run
executes two nominal `2^32`-scalar blocks per GPU; the final tail is seven scalars
shorter. Xpoint uses one X target; BSGS uses `m=257` with two signed public-key
targets. All blocks must form an exact contiguous union, and the known scalar-1
match must survive CPU verification, local durability and later authenticated
upload. Each active owner reuses one executor across grants. Completion leaves
results in the outbox until the explicit machine sync.

Timings include worker/supervisor startup, runtime self-tests, durable execution
and shutdown. Assignment setup and post-run inspection/upload are excluded.
BSGS rates describe effective scalar coverage; target-giant operations are a
different unit recorded per grant. These finite-job medians are not twelve-hour
sustained rates, kernel-only timings or a controlled comparison against MI300X.

```sh
TMPDIR=/var/tmp python3 tools/validate_fleet.py --build-dir build/cuda-c20 \
  --apache-root /tmp/keyhunt-c20-deps/root --backend cuda --counts 1,2,4,8 \
  --repeat 5 --block-bits 32 --output /var/tmp/C20_CUDA_FLEET.json
TMPDIR=/var/tmp python3 tools/validate_fleet.py --build-dir build/cuda-c20 \
  --apache-root /tmp/keyhunt-c20-deps/root --backend cuda --counts 2 \
  --lifecycle-only --reference-report /var/tmp/C20_CUDA_FLEET.json \
  --reference-device 0 --output /var/tmp/C20_CUDA_LIFECYCLE.json
```

## Recovery and decisions

The [calibrated live lifecycle report](baselines/C20_CUDA_LIFECYCLE.json) passes
with two H200 owners:

- Three authenticated server pause/resume cycles preserve both PIDs and the
  device failure budget.
- A confirmed socket pause lasts 65.04 seconds against the 60-second watchdog;
  its healthy peer continues progressing.
- A SIGSTOP-stalled owner is quarantined within the bounded watchdog/drain path,
  while its peer and an explicit machine sync continue.
- Queue 1 restarts alone with its original GPU remapped to visible ordinal 0.
- A 128-byte preparation budget fails only the BSGS owner; its xpoint peer
  completes both blocks. Journal integrity checks pass after recovery.

The [reference recommendations](baselines/C20_CUDA_CALIBRATION.json) use five
warmed single-GPU grants from physical H200 0: **44,163,351,920,315 scalars**
for xpoint, and **441,060,163,499,284 scalars** for BSGS with `m=257` and
two targets. They retain exact rational rates and algorithm alignment.

Retain the existing C18 CUDA kernel choices. This follow-up establishes a current
concurrent-worker baseline; it supplies no paired evidence for another arithmetic
or launch-geometry optimization. A twelve-hour block width is a prediction from
five warmed, validated reference grants, not a twelve-hour completion test.
Recommendations bind the reference UUID, algorithm, target set and BSGS `m` and
apply only to new immutable jobs.

The stall case stops a host process with SIGSTOP; it does not induce an actual
GPU driver hang. The memory-pressure case limits host preparation budgets;
backend refusal tests separately cover device-memory limits. MIG, distributed
coordinator/worker traffic across hosts and public ingress remain unvalidated by
this loopback session. The original frozen audit reports and C18/C20 HIP evidence
are preserved unchanged.

## Startup-setting experiment

After the suite, fleet and recovery runs finished, twelve fresh worker self-test
processes compared the default setting with `CUDA_FORCE_PRELOAD_LIBRARIES=0`,
which is described in [NVIDIA's driver initialization documentation](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__INITIALIZE.html).
Each variant has one excluded warm-up and five measured samples; order reverses
between pairs. Every process passes the same production xpoint/BSGS self-test
on GPU 7 and returns the same device identity.

The median is **3.406 seconds** with defaults and **3.424 seconds** with lazy
preloading, with overlapping sample ranges. There is no measured benefit that
justifies changing the default. These are fresh-process self-test timings,
not kernel-throughput measurements. [Raw paired observations](baselines/C20_CUDA_STARTUP.json)
and the reproduction script in the log archive retain the rejected setting.

All 35 archived reports/logs and their checksums were verified before committing.
GPU-process monitoring collected 423 two-second observations with no identified
foreign GPU executable; nine short-lived rows exited before executable lookup.
This is observed load evidence, not a reservation or proof against all brief
external activity. The full 64-test run used `fb57428`; the empty-mask test
correction and focused reruns used `29fbf63`. Production binary hashes are
identical across those revisions, and the fleet/recovery report records `29fbf63`.
