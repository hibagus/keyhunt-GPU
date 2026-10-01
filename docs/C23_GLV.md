# C23: exact-range GLV scalar multiplication

Status: implementation in progress. This slice adds an opt-in `--kernel glv`
for xpoint, Bitcoin HASH160/P2PKH, Ethereum and vanity. It is a direct-search
implementation choice; the default remains `stepped`. Legacy related-key orbit
expansion, BSGS/minikey GLV kernels and other search orders remain separate work.

## Semantics and compatibility

GLV must return exactly `kG` for each existing candidate scalar k. It does not
search extra related scalars. Forward, positive-stride and reverse mappings keep
their existing coordinates, target identities, job digests and coverage rules.
Configuration versions 1/2/3, schema 7 and receipt bytes are unchanged. Switching
between `direct`, `glv` and `stepped` at a restart is allowed because they certify
the same candidate set. Kernel choice is not immutable job semantics.

Checkpoint runs and persistent online/offline owners accept the kernel choice.
Old worker capability lists remain sufficient for their supported job mappings;
the new binary must understand `glv`. Fresh owned-device self-tests exercise it
before work starts. Minikey and BSGS commands reject unsupported kernel options.

## Arithmetic decision

The secp256k1 map `phi(x,y)=(beta*x,y)` satisfies `phi(G)=lambda*G`. Decompose k
as `k1+lambda*k2 mod n`, with signed magnitudes below `2^128`, then jointly
multiply G and phi(G). Each candidate retains its original scalar k.

The lattice basis and rounded reciprocal constants are documented in the
[pinned libsecp256k1 scalar implementation](https://github.com/bitcoin-core/secp256k1/blob/0cdc758a56360bf58a851fe91085a327ec97685a/src/scalar_impl.h).
The device implementation uses 32-bit limbs, full 512-bit products for rounded
quotients and checked signed 128-bit components. This keeps scalar arithmetic
separate from the field modulus. The pinned library remains a test-only oracle;
production does not call or link it. CPU result verification remains independent.

## Acceptance gates

Independent Python integers check decomposition, rounding boundaries, sign
combinations and invalid scalars. Pinned public-key oracles check full affine
points, including wide inputs and curve-order endpoints, on portable C++, HIP
and CUDA. Search gates cover all four families, both public-key encodings,
forward/reverse/unit/nonunit strides, short tails, overflow, maximum batches and
all visible ordinals. Existing direct/stepped paths retain regression checks.

Durable gates switch kernels across restarts, pause and changed visibility,
verify exact results and exercise HTTPS and disconnected file transport. Paired
measurements compare direct, GLV and stepped kernels in one process with warmup,
repeated raw samples, fixed candidates/targets, allocation and binary/device
metadata. Any speed claim is limited to those measurements; the default changes
only with separate evidence. Decisions, failures and acceptance evidence belong
under docs/, with logical implementation changes committed separately.

## Arithmetic implementation finding

Portable C++ and native HIP passed 5,973 oracle cases: 5,328 decomposition inputs,
603 full public keys, 36 endomorphism points/alias checks and invalid-key checks.
The integer corpus exercises all four sign combinations and values immediately
around reciprocal rounding boundaries. Production computes signed lattice
residuals using low 256-bit two's-complement words and rejects magnitudes outside
the proven 128-bit bound. Independent point verification confirms the scalar
relation, rather than accepting a decomposition solely because it is small.

## Search integration finding

Native HIP passed 144 independent four-family search cases across forward and
reverse order, including unit strides, near-order values, 1,048,576-candidate
batches, overflow replay and all eight visible ordinals. Executor tests retain
every-index tails, ownership checks and runtime/download corruption gates for
all three kernels. The new sparse unit-reverse fixture initially included an
in-range negative target; moving it outside the interval corrected the expected
set. Both complete CLI matrices passed after that fixture correction.
