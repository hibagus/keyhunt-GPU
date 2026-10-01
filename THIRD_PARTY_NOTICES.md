# Source provenance and retained notices

C02 reorganizes code already present at `2134a20`; it does not relicense it or
import implementation from the CUDA/Apple reference checkout. The root
[LICENSE](LICENSE) contains Luis Alberto's MIT notice. Individual source files
also carry the notices below; the root MIT notice does not replace them.

| Current location | Original location | Notices present in this checkout |
| --- | --- | --- |
| `src/crypto/secp256k1/`, `include/keyhunt/crypto/secp256k1/` | `secp256k1/` | Jean Luc PONS, BSGS; source headers specify GNU GPL version 3 |
| `src/crypto/hash/`, `include/keyhunt/crypto/hash/` | `hash/` | Jean Luc PONS, VanitySearch; source headers specify GNU GPL version 3 |
| `legacy/gmp256k1/` | `gmp256k1/` | Mixed per-file notices: Luis Alberto MIT and Jean Luc PONS GPLv3; retain each source header |
| `third_party/base58/` | `base58/` | Luke Dashjr 2012–2014; source declares standard MIT and references a COPYING file absent from the original checkout |
| `third_party/bloom/`, `third_party/oldbloom/` | `bloom/`, `oldbloom/` | Jyri J. Virkki; retained two-clause BSD license files, with local keyhunt adaptations |
| `third_party/xxhash/` | `xxhash/` | Yann Collet; retained upstream LICENSE distinguishes BSD-2-Clause library from GPLv2-or-later xxhsum utility; this build compiles the library |
| `third_party/sha3/` | `sha3/` | Taylor R. Campbell 2015; retained two-clause BSD notices embedded in source/header files |
| `third_party/rmd160/` | `rmd160/` | References Bosselaers' RIPEMD-160 implementation and Martin Hinner's adaptation; no standalone license was present in the original directory |

The test-only `third_party/secp256k1-oracle/` dependency was added in C06 from
Bitcoin Core libsecp256k1 v0.6.0, commit `0cdc758a56360bf58a851fe91085a327ec97685a`.
It carries the upstream [MIT notice](third_party/secp256k1-oracle/COPYING).
[Provenance and scope](docs/ARITHMETIC_ORACLE.md) and the
[per-file source pin](tests/oracle/secp256k1.lock.json) describe the import.
It is not linked into the search executable. The retained upstream Wycheproof
fixtures have their separate [Apache 2.0 notice](third_party/secp256k1-oracle/src/wycheproof/WYCHEPROOF_COPYING);
those fixtures are not compiled by this integration.

C12 links an external SQLite library supplied by the build environment; no
SQLite implementation is vendored. The [storage setup](docs/STORAGE.md#database-and-deployment-boundary)
records the required version and the installed standalone library used for validation.
Deployment packages must provide that runtime dependency.

C15 optionally compiles against external nlohmann JSON headers (3.10.5 used in
validation). Their MIT notice and the additional header contributor notices are
retained in [NLOHMANN_JSON.txt](third_party/licenses/NLOHMANN_JSON.txt), copied from
the supplied headers and Ubuntu `nlohmann-json3-dev` package copyright record.
No JSON implementation is vendored. C15 links external OpenSSL 3 (`libcrypto`)
and, for HTTPS workers only, libcurl. Deployment must provide those runtime
libraries with their upstream/package notices. Apache is an external service;
the repository supplies configuration and test fixtures, not Apache code.

The `src/crypto/` placement identifies maintained CPU implementation, not
original authorship. All source notices and existing dependency licenses remain
with their files. [GPL-3.0.txt](third_party/licenses/GPL-3.0.txt) supplies the full
GPLv3 text referenced by the existing headers (copied from the host's
`/usr/share/common-licenses/GPL-3`). Embedded SHA-3 notices are also reproduced
in [SHA3.txt](third_party/licenses/SHA3.txt).

This inventory records evidence in the checkout, not a determination that the
whole linked executable is MIT-only. Resolve incomplete upstream licensing
records before distributing binary packages. Future imported code requires its
own provenance and compatible notices; the fixture generator and new build
files do not change the licensing of existing code.

C23's separate [GLV device arithmetic](kernels/common/glv.h) uses the published
secp256k1 lattice and reciprocal constants described by Pieter Wuille's
`scalar_impl.h` in that pinned MIT-licensed source. Its notice is retained in the
header and the upstream [COPYING](third_party/secp256k1-oracle/COPYING). The
32-bit product/signed-component implementation is local; the oracle library
remains test-only and is not linked into production.
