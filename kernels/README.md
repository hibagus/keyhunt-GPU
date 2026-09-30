# Device kernels

C08 supplies portable integer primitives in `common/field.h` and `common/point.h`:
canonical secp256k1 field operations, zero-safe batch inversion, Jacobian points,
and scalar-domain checks. The same headers compile on CPU and HIP. See the
[arithmetic contract and oracle results](../docs/GPU_ARITHMETIC.md).

C07's HIP transport/index diagnostic remains in `hip/diagnostic.h`. It validates
bounded launches and is not a search. See the [HIP execution contract](../docs/HIP_BACKEND.md).
Arithmetic test and measurement entry points live under `tests/gpu/`.

C09/C11 add search kernels; C18 adds native CUDA runtime validation. Measured ISA
specializations belong in `arch/` after their separate gates. CPU legacy headers
and x86 assembly remain under `include/` and `src/crypto/`.
