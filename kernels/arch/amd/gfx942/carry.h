#pragma once
#include <cstdint>

// Opt-in compiler intrinsics are restricted to the validated gfx942 device
// pass. Host, CUDA, and other HIP architectures retain portable arithmetic.
#if defined(KEYHUNT_GFX942_CARRY) && defined(__HIP_DEVICE_COMPILE__) && \
    defined(__gfx942__) && defined(__clang__)
#if __has_builtin(__builtin_addc) && __has_builtin(__builtin_subc)
#define KEYHUNT_USE_GFX942_CARRY 1
namespace keyhunt::gpu::gfx942 {
// Clang keeps the word-to-word carry in VCC and materializes only the final
// 0/1 flag. Compiler intrinsics describe those dependencies without assembly
// register constraints or clobbers. Exact input/output aliasing is supported:
// each limb is read before its store and later limbs use distinct locations.
__device__ inline __attribute__((always_inline))
void add_words(uint32_t* out, const uint32_t* a, const uint32_t* b, uint32_t& flag) {
    unsigned carry = 0;
    for (unsigned i = 0; i < 8; ++i)
        out[i] = __builtin_addc(a[i], b[i], carry, &carry);
    flag = carry;
}

__device__ inline __attribute__((always_inline))
void subtract_words(uint32_t* out, const uint32_t* a, const uint32_t* b, uint32_t& flag) {
    unsigned borrow = 0;
    for (unsigned i = 0; i < 8; ++i)
        out[i] = __builtin_subc(a[i], b[i], borrow, &borrow);
    flag = borrow;
}
} // namespace keyhunt::gpu::gfx942
#endif
#endif
