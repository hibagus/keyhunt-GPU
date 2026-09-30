# Portable GPU arithmetic (C08)

C08 introduces independent device arithmetic under `kernels/common/`. It is
shared source compiled as C++17 and HIP, with no runtime headers, x86 assembly,
warp-size assumptions or floating-point operations. CUDA execution still needs
C18's native-toolchain and hardware gates. These are arithmetic primitives, not
a GPU search, result-verification bypass or completed-coverage mechanism.

## Field representation and reduction

`Field` uses eight little-endian 32-bit limbs, always canonical in `[0,p)`, where
`p = 2^256 - 2^32 - 977`. The external representation is 32-byte big endian.
`from_bytes_checked` rejects values at or above p and clears its output;
`from_bytes_reduced` reduces any 256-bit input. Arithmetic operands must be
canonical. Raw limb layouts are not a portable cache format.

The schoolbook multiply retains all sixteen product limbs. Each inner sum is
bounded by `(2^32-1)^2 + 2*(2^32-1) = 2^64-1`. Reduction folds the high half
using `2^256 = 2^32 + 977 (mod p)`, retaining the shifted high limb separately.
The residual high value is at most `2^32+977`. Its first fold adds less than
`2^66`; any carry out of bit 255 leaves a small low result, so a second fold
cannot carry again. A final conditional subtraction gives the unique canonical
representative. This avoids the reference implementation's overflowing 33-bit
constant multiplication. No reference CUDA arithmetic was copied.

Add, subtract, multiply, square, negate and inverse accept output aliasing either
input, including `mul(a,a,a)`. Inputs are consumed before assigning output.
Inversion uses exponentiation by p-2. Zero returns false and writes zero, including
in place. The bounded `batch_inverse<Capacity>` skips zeros in its prefix product,
outputs zero in their positions, and supports exact in-place arrays. A count
above Capacity returns false before touching the output; arbitrary partially
overlapping arrays are outside the contract. An empty group succeeds.

This is variable-time arithmetic for public search inputs. It is not a
constant-time signing or secret-key handling API. Fermat inversion and serial
batch inversion are a correctness baseline, with optimization deferred until
measured. The 8x32 layout is provisional, not an asserted MI300X optimum.

## Field differential validation

`portable_field_oracle` compiles the actual headers on the host; `hip_field_oracle`
executes them on gfx942. Both compare canonical results to exact Python integers,
and compare the canonical-input subset with the preserved CPU implementation.
The corpus includes prime boundaries, full-width raw ingress, limb carries,
randomized multiplication, aliases, inverse status, serialization and zero-aware
batch inverses. Additional launch sizes 1/127/128/129/256/257/1024 exercise full
and partial workgroups. Every HIP batch checks a device execution counter and a
guard after the last result. A successful launch alone cannot pass the test.

```sh
cmake --preset hip-release
cmake --build --preset hip-release --parallel 4
ctest --preset hip-release -R '^(portable|hip)_field_oracle$'
cmake --preset cpu-sanitizers
cmake --build --preset cpu-sanitizers --parallel 4 \
  --target portable_arithmetic_probe cpu_arithmetic_probe
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset cpu-sanitizers -R '^portable_field_oracle$'
```

The probe's streaming batches bound allocations to at most 1,024 requests. Its
synchronous transfer path and default stream are test infrastructure; production
search integration must retain C07's explicit ownership/backpressure contract.
The event and host times in these reports are correctness-run measurements, not
a benchmark or end-to-end key throughput. [Field evidence](baselines/C08_FIELD.json)
records the HIP, host and sanitizer results. Point arithmetic follows in a
separate C08 change.
