# Device kernels

C07 ships the HIP index/transport diagnostic in `hip/diagnostic.h`. It validates
bounded launches and is not a search or cryptographic primitive. See the
[HIP backend contract](../docs/HIP_BACKEND.md).

Portable arithmetic/search headers will live in `common/`, HIP and CUDA entry
points in `hip/` and `cuda/`, and measured ISA specializations in `arch/`.
CPU headers and x86 assembly remain under `include/` and `src/crypto/`.
