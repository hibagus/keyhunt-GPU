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

## Points, scalars and exceptional cases

`Point` uses Jacobian coordinates `x = X/Z^2, y = Y/Z^3`, not the legacy CPU
engine's homogeneous projective representation. Every Z=0 input is infinity;
point operations produce the canonical all-zero infinity. Finite inputs must
already be valid curve points with canonical field coordinates. `on_curve`
checks canonical affine coordinates against `y^2 = x^3 + 7`; it is not a
compressed-key parser. `from_affine` assumes that validation has already happened.

The addition formula computes scaled U/S coordinates, then H=U2-U1 and R=S2-S1.
H=0 dispatches to doubling when R=0, or infinity otherwise. Doubling handles
infinity and Y=0 explicitly. The general formulas and Jacobian convention are
standard [short Weierstrass arithmetic](https://hyperelliptic.org/EFD/g1p/auto-shortw-jacobian.html);
the implementation and special-case handling were written for this repository.
Outputs are assigned after all input reads, preserving both input aliases.
Mixed addition initially wraps the same complete general path with affine Z=1.
It has no separately advertised speed advantage.

`Scalar` stores unsigned 256-bit scalar bits separately from field elements.
`point_multiply` handles all bit patterns, including zero and n, by bounded
256-bit double-and-add; the group law naturally accounts for the curve order.
`public_key` rejects zero and values >=n, clears its output on failure, and uses
the fixed generator on success. It does not reduce rejected private scalars or
apply field reduction to scalar bits. This API does not yet supply arbitrary
scalar modular add/multiply primitives, window tables or constant-time signing.

## Point differential validation

`portable_point_oracle` and `hip_point_oracle` compare public scalar derivation,
Jacobian normalization, negation, doubling, general/mixed addition and arbitrary
point multiplication. The pinned libsecp256k1 probe supplies public keys and
point sums; Python's independent affine model agrees with those results and
supplies the other expected values. The legacy CPU adapter is also compared.

Cases include zero/n/n+1/maximal scalars, high-bit scalars, infinity, equal/opposite
points, differing nontrivial projective scales, left/right/self aliases, invalid
affine coordinates, canonical infinity output, and partial point workgroups.
Field tests additionally check that empty/oversized inversion groups preserve
output ownership. The GPU returns canonical affine results computed on the GPU;
host-side serialization does not redo curve math to conceal a device failure.

```sh
ctest --preset hip-release -L arithmetic
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset cpu-sanitizers -L arithmetic
```

[Point evidence](baselines/C08_POINT.json) contains both point reports, focused
field/point CTest output and sanitizer results. No legacy arithmetic or range
semantics were changed in this milestone.
