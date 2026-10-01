# GPU operations

Use the [build matrix](BUILD.md#validated-gpu-builds) and
[finite quickstart](GPU_QUICKSTART.md) to verify the chosen native backend first.
The [mode matrix](MODES.md) covers exact xpoint, BSGS and Bitcoin P2PKH/HASH160.
Standalone execution needs only `keyhunt`; authenticated workers additionally
need the optional coordinator/HTTPS build. HTTPS workers hold enrolled client
credentials; [file-only workers](OFFLINE_ASSIGNMENTS.md) keep those credentials
on a connected courier.

## State, inputs and device identity

Keep journals in private, absolute directories on local storage outside every Git
checkout. SQLite needs space for its WAL, retained receipts and sealed migration
backups. `state compact` reclaims pages; it does not discard audit history. Keep
canonical targets and BSGS table files with the deployment. Legacy `-S` files are
only CPU caches and cannot replace these tables or a progress journal.

Choose the visible device ordinal explicitly. A visibility mask can renumber it;
the runtime UUID identifies the selected logical device. MI300X partition changes
require stopped workers and rediscovery. Current fleet results use eight physical
MI300X in SPX/NPS1 or eight physical H200 with MIG disabled. QPX search, CPX fleet
scaling and CUDA MIG are not certified by those measurements. Never multiply one
partition's memory or rate by a logical count to claim package capacity.

A job fixes its half-open scalar range, block width, canonical targets and BSGS
configuration. Device choice and launch geometry can change on restart; changing
targets, HASH160 encoding selection or table identity requires a new job. `state claim --policy sequential`,
`random`, `random-window` or `manual` selects unexplored blocks, not random scalar
samples inside an active block. Completed and in-progress blocks are excluded.
See [selection and fences](STORAGE.md#local-commands).

## Concurrent workers

First exercise the [isolated localhost setup](COORDINATOR.md#s06-isolated-localhost-operation).
It creates real test certificates, approved identities and a tiny job, with one
queue per worker. Keep its test CA private and confined to that environment.
Public ingress, ACME and physical cross-host coordinator traffic remain unvalidated.
SSH execution on the H200 node is hardware validation, not cross-host coordinator
validation.

For an existing authorized job, a **new** worker's private JSON configuration has
this shape (replace every placeholder with the actual enrolled deployment value):

```json
{
  "endpoint": "https://coordinator.example:443",
  "ca": "/private/worker/server-ca.pem",
  "certificate": "/private/worker/client.pem",
  "key": "/private/worker/client.key",
  "jobs": [{
    "project": "PROJECT_UUID",
    "job": "JOB_SHA256",
    "devices": ["0", "1"],
    "spares": 1,
    "policy": "sequential"
  }]
}
```

Use `random` or `random-window` for a different block-selection policy. `spares`
is 0 or 1 per queue. Queue IDs must be unique across this machine's jobs; default
mapping treats each ID as a visible ordinal. Non-numeric IDs need `--device-map`.
Credential paths are absolute; the key must be private and owned by the worker
user. The public certificate must be registered with an authorized project role.
Private-key content is never an enrollment field. Configuration is immutable once
saved; editing the original JSON does not reconfigure an existing journal.

With `keyhunt-worker`, `keyhunt-supervise` and `keyhunt` installed on PATH, set
`worker_state` to a new private directory, `worker_config` to the completed JSON
file, and `backend` to `hip` or `cuda`, then:

```sh
keyhunt-worker configure --state-dir "$worker_state" --config "$worker_config"
keyhunt-worker sync --state-dir "$worker_state"
keyhunt-supervise --state-dir "$worker_state" --backend "$backend" \
  --worker "$(command -v keyhunt-worker)" --keyhunt "$(command -v keyhunt)" \
  --devices 0,1 --once
keyhunt-worker status --state-dir "$worker_state"
```

For a source build use `python3 tools/coordinator_worker.py` and explicit binary
paths. BSGS also requires `--table /absolute/path/babies.khb` matching the job's
checksum. One supervisor invocation supplies one table path, so all BSGS jobs it
runs must accept that table. Xpoint does not use it. `--once` consumes executable
saved queues and exits without an extra final synchronization. Omit `--once` for
a continuing supervisor. Each persistent device process self-tests once and
retains targets/tables across grants. Faster devices can take only unstarted,
unclaimed grants of the same job. An omitted queue retains its active block.

The default host table/target budget is 1 GiB per device and 8 GiB in aggregate;
`--host-memory` and `--host-memory-total` adjust these byte limits. Each selected
owner gets at most the smaller of its per-device cap and an equal share of the
aggregate cap. Runtime contexts, SQLite and Python use additional memory. Device
allocation still checks current free memory and reserve headroom.

## Controls, restart and acknowledgment

For a running queue, use:

```sh
keyhunt checkpoint pause --state-dir "$worker_state" --slot 0
keyhunt checkpoint status --state-dir "$worker_state" --slot 0
keyhunt checkpoint resume --state-dir "$worker_state" --slot 0
```

Wait for `durably_paused:true` before relying on a pause. `SIGINT`/`SIGTERM` to the
supervisor requests a fleet drain and checkpoint; `SIGUSR1`/`SIGUSR2` controls all
selected owners. Standalone `checkpoint run` uses the same controls without
`--slot`. A pause retains ownership and does not extend the assignment's expiry.
The [control guide](PAUSE_RESUME.md) explains latency, signal and socket semantics.

For restart with a changed visibility mask, stop the previous supervisor/owners
first. If queue 1's same GPU is now the only visible device, restart that queue
with `--devices 1 --device-map 1=0`. A different UUID requires the additional
`--rebind-device 1`; it cannot bypass a surviving owner's lock. Keep the journal
and valid grant. Do not delete lock files or mint new assignments to work around
ownership checks. Three execution failures or a progress stall quarantine a
queue; repair the cause before using `--retry-failed`. Healthy owners and the
separate scheduled-sync process continue for HTTPS workers. No automatic GPU reset
is performed.

HTTPS workers persist a machine-wide 7,200-second contact schedule and default
30-day leases. Completion, matches and restart do not accelerate this schedule.
A reboot
or uncertain monotonic deadline requires authenticated revalidation before more
work. A server pause reaches an offline worker only when it next contacts the
server; use local controls for immediate intervention on that host. File-only
workers create no scheduled-sync child and report `sync_due_in:null`. Their
[manual exchange](OFFLINE_ASSIGNMENTS.md#run-a-disconnected-worker) carries
assignments, outbox pages and acknowledgments through a connected courier; file
delivery consumes lease time. Reboots require a fresh export/relay/import.

`keyhunt-worker status` reports local completion, pending outbox, last server
acknowledgment, lease and sync state. For HTTPS workers, request an earlier upload
explicitly:

```sh
keyhunt-worker sync --state-dir "$worker_state"
```

One sync transfers a bounded page set; remaining backlog waits for another manual
or scheduled contact. Until acknowledged, results are locally durable only. The
default 64 MiB outbox stops further execution when exhausted rather than dropping
unacknowledged data. Losing the local disk can lose work since the last successful
sync, including longer outages. Receipt history also grows outside the outbox cap.

After an abrupt process exit, inspect `state check`, `state block` and
`checkpoint results`, then resume the retained valid assignment. Accepted coverage
survives; uncommitted work replays and matches deduplicate. For expired/transferred
remote work, use the [coordinator recovery operation](COORDINATOR.md#s05-recovery-and-coordinator-only-installation)
after the previous executor stops. Raw local `state` commands cannot extend a
remote lease. A process stuck in a driver may retain its locks even after a kill
request; the stopped-process watchdog test does not prove recovery from a physical
driver hang. Restore snapshots remain quarantined until the documented authority,
executor and credential reconciliation is complete. Copying a live SQLite main
file without its WAL is not a supported backup.

## Interpreting performance

| Measurement | Use and evidence |
| --- | --- |
| Kernel events | Compare identical arithmetic/search variants; [HIP C17](HIP_TUNING.md), [gfx942 C19](GFX942_SPECIALIZATIONS.md), [CUDA C18](CUDA_BACKEND.md) include raw artifacts and rejected experiments |
| Standalone process time | Includes startup, inputs, output, verification and durability costs; [C16 methodology](GPU_PROFILING.md) distinguishes periodic commits from final-only commits |
| Persistent fleet time | Includes fresh worker self-tests, durable execution and shutdown; [MI300X raw fleet](baselines/C20_FLEET.json) and [H200 raw fleet](baselines/C20_CUDA_FLEET.json) have 1/2/4/8 physical GPUs, one warm-up and five samples per case |
| New-job block sizing | [Calibration](MULTI_GPU.md#work-units-and-reference-calibration) derives immutable widths from five validated warmed single-device grants of matching inputs |

Fleet samples use small tables and finite jobs. Their twelve-hour block sizes
are predictions, not twelve-hour endurance results. HIP and CUDA runs are not a
controlled cross-vendor comparison. Xpoint scalars/s and BSGS effective scalar
coverage/s are different work measures; BSGS reports actual target giant steps
separately. Changing target count, `m`, partitioning, launch geometry or sibling
load can change rates. Retain raw results, UUIDs, toolchain and exact settings.

Keep C19 carry intrinsics opt-in; accepted CUDA tuning is already the native
default. No clock, power, partition, register-cap or CUDA startup-environment
change is required by these guides. Run timing trials after correctness tests,
without another benchmark competing for the same devices.
