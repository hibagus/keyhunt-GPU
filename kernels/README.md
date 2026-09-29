# Device kernels

Device implementation begins at C07/C08. No GPU kernel is shipped yet.
Portable arithmetic/search headers will live in `common/`, HIP and CUDA entry
points in `hip/` and `cuda/`, and measured ISA specializations in `arch/`.
CPU headers and x86 assembly remain under `include/` and `src/crypto/`.
