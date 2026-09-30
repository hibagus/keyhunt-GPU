#include <hip/hip_runtime.h>
#include "../kernels/arch/amd/gfx942/carry.h"
#if defined(__HIP_DEVICE_COMPILE__) && defined(__gfx942__) && !defined(KEYHUNT_USE_GFX942_CARRY)
#error "The selected compiler cannot enable the gfx942 carry primitive"
#endif

// Compile actual device calls for both carry and borrow intrinsics. No GPU
// is required at configure time; this probe builds only a static library.
__global__ void keyhunt_check_carry(uint32_t* out, const uint32_t* a, const uint32_t* b) {
#if defined(KEYHUNT_USE_GFX942_CARRY)
    uint32_t flag;
    keyhunt::gpu::gfx942::add_words(out, a, b, flag);
    out[8] = flag;
    keyhunt::gpu::gfx942::subtract_words(out + 9, a, b, flag);
    out[17] = flag;
#else
    // The host pass and other code objects in a mixed HIP build need no ISA.
    (void)out; (void)a; (void)b;
#endif
}
