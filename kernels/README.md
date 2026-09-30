# Device kernels

C08 supplies portable integer primitives in `common/field.h` and `common/point.h`:
canonical secp256k1 field operations, zero-safe batch inversion, Jacobian points,
and scalar-domain checks. The same headers compile on CPU and HIP. See the
[arithmetic contract and oracle results](../docs/GPU_ARITHMETIC.md).

C07's HIP transport/index diagnostic remains in `hip/diagnostic.h`. It validates
bounded launches and is not a search. See the [HIP execution contract](../docs/HIP_BACKEND.md).
Arithmetic test and measurement entry points live under `tests/gpu/`.

C09 implements direct and stepped xpoint kernels in `hip/xpoint.h`, with exact
lookup, bounded candidate writes and checked tail counts. See the
[xpoint contract and measurements](../docs/HIP_XPOINT.md). C10 supplies the portable full-point lookup/filter in
`include/keyhunt/core/bsgs_layout.h` and the HIP preparation probe in
`hip/bsgs_probe.h`. C11 implements the signed residual, giant stepping and
bounded candidate kernel in `hip/bsgs_search.h`; see [the search contract](../docs/HIP_BSGS.md).
C18 adds native CUDA runtime validation. Measured ISA
specializations belong in `arch/` after their separate gates. CPU legacy headers
and x86 assembly remain under `include/` and `src/crypto/`.

C18: shared search kernels now live in `search/`; `device_runtime.h` is a private
native HIP/CUDA SDK spelling adapter. Public host APIs remain SDK-free. See
[the CUDA guide](../docs/CUDA_BACKEND.md) for H200 builds and validation.
