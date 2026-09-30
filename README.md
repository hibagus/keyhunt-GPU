<a id="readme-top"></a>

# keyhunt-GPU

A staged redesign of keyhunt for AMD HIP and NVIDIA CUDA, with reproducible
correctness checks, resumable searches, and coordinated work across machines.

**Current status:** the CPU engine has been characterized and reorganized.
HIP discovery, arithmetic and bounded xpoint searches pass on MI300X, with CPU
verification and candidate overflow replay. Versioned BSGS tables and bounded
HIP BSGS range searches are implemented, including all targets and exact tails.
The local journal now supports transactional assignments and CPU-verified durable
HIP checkpoints, graceful local pause/resume and checkpoint-on-signal shutdown.
Authenticated coordination now adds mTLS, project roles, durable machine sync,
an offline outbox and fenced recovery, validated in an isolated localhost setup.
Native CUDA now supports the same bounded xpoint/BSGS and local checkpoint
contracts, validated on eight H200s with measured arithmetic and inline PTX tuning.

[Build guide](docs/BUILD.md) · [Usage](docs/USAGE.md) ·
[Implementation status](docs/IMPLEMENTATION_STATUS.md) ·
[Report an issue](https://github.com/hibagus/keyhunt-GPU/issues)

| Capability | Current state |
| --- | --- |
| Linux x86-64 CPU | Release and debug builds tested; 38 characterization checks pass |
| CPU modes | Bitcoin address/HASH160, xpoint, BSGS, Ethereum address, vanity and minikeys; limitations documented |
| Optional GMP legacy / bsgsd | Builds and selected compatibility checks pass; separate from the authenticated coordinator |
| AMD HIP / MI300X | C07–C11 discovery, arithmetic, xpoint and BSGS searches validated on gfx942 |
| NVIDIA CUDA | Native C18 backend; H200 build and validation in [CUDA_BACKEND.md](docs/CUDA_BACKEND.md) |
| Local state | C12 project-scoped SQLite journal, sparse allocation, fenced assignments and sealed backups; [operations guide](docs/STORAGE.md) |
| Durable GPU searches | C13/C18 verified local checkpoints and replay for HIP/CUDA xpoint and BSGS; [commands and recovery](docs/CHECKPOINTS.md), [CUDA usage](docs/CUDA_BACKEND.md) |
| Pause/resume | C14 local commands, graceful signals, exact restart and live inspection; [operations guide](docs/PAUSE_RESUME.md) |
| Distributed blocks | C15 authenticated coordination and supervised workers; [localhost setup](docs/COORDINATOR.md#s06-isolated-localhost-operation). Simultaneous multi-GPU execution remains C20 |

The CPU engine still has range, stride and whole-application sanitizer defects.
C06 corrected arithmetic and the tested BSGS start-boundary miss. See
[baseline findings](docs/CPU_BASELINE.md) for the remaining coverage limitations.

<details>
<summary>Table of contents</summary>

- [About the project](#about-the-project)
- [Built with](#built-with)
- [Getting started](#getting-started)
- [Usage](#usage)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)
- [Contact](#contact)
- [Acknowledgments](#acknowledgments)

</details>

## About the project

This repository builds on [AlbertoBSD's keyhunt](https://github.com/albertobsd/keyhunt)
for secp256k1 search experiments and public puzzle fixtures. The redesign separates
application code, shared headers, CPU arithmetic, device kernels and work storage.
AMD HIP and native NVIDIA CUDA implement the shared arithmetic and bounded
search contracts, with hardware-specific tuning validated independently.

The distribution design gives each selected GPU its own block, sized for roughly
12 hours on a reference device, with local checkpoints and a 30-day renewable
assignment. The C15 supervisor batches synchronization with an authenticated
HTTPS coordinator every two hours. Block width remains an explicit calibration
input; production execution is one GPU at a time until C20. Public ingress and a
physical second host are deferred from the user-approved localhost gate. See the
[GPU plan](docs/GPU_REDESIGN_PLAN.md) and [coordinator plan](docs/COORDINATOR_SERVER_PLAN.md).

## Built with

- C++17 and C, CMake 3.22+, and POSIX threads.
- Existing CPU secp256k1, hashing, Bloom filter and encoding implementations;
  [source provenance and notices](THIRD_PARTY_NOTICES.md) are retained.
- Python's standard library for the regression and benchmark harnesses.
- SQLite 3.51.3+ development headers/library for durable local state.
- GMP and OpenSSL for the optional legacy executable.
- OpenSSL 3 and nlohmann JSON for optional coordination; libcurl for HTTPS
  workers and Apache for the mTLS deployment boundary.

ROCm/HIP is optional for [device diagnostics](docs/HIP_BACKEND.md) and
[bounded xpoint searches](docs/HIP_XPOINT.md). Native NVIDIA builds use the [H200 CUDA preset](docs/CUDA_BACKEND.md).
CPU builds do not require a GPU SDK or network access.

## Getting started

### Prerequisites

Use Linux x86-64 with SSSE3, GCC/G++, Make, CMake 3.22 or newer, and Python 3.9 or
newer for tests, plus SQLite 3.51.3+ headers/library. See the
[SQLite setup](docs/STORAGE.md#database-and-deployment-boundary) for explicit
dependency paths on the validated host. The validated host uses GCC 11.4.0,
CMake 3.22.1 and Python 3.10.12.
Native CPU tuning is enabled by default; see [build options](docs/BUILD.md#options)
when building for a different machine.

### Installation

```sh
git clone https://github.com/hibagus/keyhunt-GPU.git
cd keyhunt-GPU
cmake --preset cpu-release
cmake --build --preset cpu-release --parallel 4
ctest --preset cpu-release
```

The executable is `build/cpu-release/keyhunt`. `make` remains a compatibility
entry point and places a copy at `./keyhunt`. Debug, sanitizer, optional legacy,
daemon and local install instructions are in [BUILD.md](docs/BUILD.md).

## Usage

Run a small deterministic search against the repository's public solved-puzzle
fixtures, from the repository root:

```sh
./build/cpu-release/keyhunt -m address -f tests/1to32.txt \
  -r 1:401 -n 1024 -l compress -t 1 -q -s 0
```

This aligned range scans 1,024 scalar positions and then exits. Matches are
appended to `KEYFOUNDKEYFOUND.txt` in the working directory, including the known
scalar-1 address `1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH`.

Read the [usage guide](docs/USAGE.md) for input formats, bounded BSGS examples,
output files and current interruption behavior. The [mode reference](docs/MODES.md)
distinguishes active and removed modes. Avoid treating `-S` caches or a successful
process exit as proof of saved search progress or exact coverage.

## Roadmap

- [x] C01: reproducible CPU behavior and environment baseline.
- [x] C02: source/header organization, CMake and optional target validation.
- [x] C03: README redesign and current usage guides.
- [x] C04: shared host configuration, target loaders and CPU result verification.
- [x] C05: exact 256-bit ranges, immutable blocks and bounded work planning.
- [x] C06: independent arithmetic/search oracle and CPU arithmetic corrections.
- [x] C07: HIP device discovery and bounded asynchronous diagnostics.
- [x] C08: portable GPU field/point arithmetic with independent oracle validation.
- [x] C09: bounded HIP xpoint search, CPU verification and overflow replay.
- [x] C10: versioned BSGS tables, validated caches and HIP filter/exact lookup.
- [x] C11: bounded HIP BSGS searches, all targets, exact tails and overflow replay.
- [x] C12: sparse local coverage repository, transactional assignments and state commands.
- [x] C13: CPU-verified durable HIP checkpoints and exact replay after interruption.
- [x] C14: graceful local pause/resume, signals, inspection and device-independent restart.
- [x] C15: authenticated coordination, durable machine sync, offline leases and localhost validation.
- [x] C16: [reproducible GPU profiling and durability benchmarks](docs/GPU_PROFILING.md).
- [x] C17: [measured HIP arithmetic, buffer and overflow tuning](docs/HIP_TUNING.md).
- [x] C18: native CUDA, H200 parity/recovery, measured arithmetic and inline PTX tuning.
- [x] C19: [opt-in gfx942 carry/borrow intrinsics, portable fallback and paired ISA evidence](docs/GFX942_SPECIALIZATIONS.md).
- [ ] C20: simultaneous multi-GPU scheduling and balancing.
- [ ] C21–C23: operations guides, offline assignments and further GPU modes.

See [implementation status](docs/IMPLEMENTATION_STATUS.md) for evidence and
[acceptance gates](docs/GPU_REDESIGN_PLAN.md#10-commit-sized-implementation-sequence)
for each commit-sized milestone. Initial paired xpoint measurements and their
limits are recorded in [HIP_XPOINT.md](docs/HIP_XPOINT.md).

## Contributing

Open an [issue](https://github.com/hibagus/keyhunt-GPU/issues) describing the
problem and a small reproducible input, or submit a focused pull request.
Keep one logical change per commit, record analysis in `docs/`, and include the
relevant validation. Separate mechanical moves, correctness fixes and tuning.
Preserve source notices and label untested backends explicitly.

For CPU changes, run `ctest --preset cpu-release`. Known-defect baseline cases
record historical behavior; changes that fix them need explicit strict behavior
tests and an updated baseline explanation.

## License

The root [LICENSE](LICENSE) contains the original MIT notice. Existing arithmetic
and hash sources also contain GPLv3 notices; other bundled components have their
own notices. Consult [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The README
template's license does not replace any project or dependency license.

## Contact

Use the [repository issue tracker](https://github.com/hibagus/keyhunt-GPU/issues)
for questions, bug reports and proposed work.

## Acknowledgments

- [AlbertoBSD/keyhunt](https://github.com/albertobsd/keyhunt), the original CPU project.
- Jean Luc PONS and the authors named in the retained arithmetic, hash and vendor notices.
- [Best-README-Template](https://github.com/othneildrew/Best-README-Template), adapted
  for this README's structure.

<a id="download-and-build"></a>
<a id="tldr"></a>
<a id="free-code"></a>
<a id="disclaimer"></a>
<a id="for-regular-users"></a>
<a id="beta"></a>
<a id="modes"></a>
<a id="experimental-modes"></a>
<a id="address-mode"></a>
<a id="vanity-search"></a>
<a id="rmd160-mode"></a>
<a id="xpoint-mode"></a>
<a id="endomorphism"></a>
<a id="pub2rmd-mode"></a>
<a id="bsgs-mode-baby-step-giant-step"></a>
<a id="file-creation"></a>
<a id="examples"></a>
<a id="valid-n-and-maximun-k-values-for-specific"></a>
<a id="what-values-use-according-to-my-current-ram"></a>
<a id="testing-puzzle-63-bits"></a>
<a id="is-my-speed-real"></a>
<a id="minikeys-mode"></a>
<a id="ethereum"></a>
<a id="speeds"></a>
<a id="faq"></a>
<a id="thanks"></a>
<a id="donations"></a>
<a id="testnet"></a>

Older README sections are preserved in the [historical CPU README](docs/HISTORICAL_README.md).
Use the current [mode reference](docs/MODES.md), [usage guide](docs/USAGE.md), and
[build guide](docs/BUILD.md) for validated instructions.
