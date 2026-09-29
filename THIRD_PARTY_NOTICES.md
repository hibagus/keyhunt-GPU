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
