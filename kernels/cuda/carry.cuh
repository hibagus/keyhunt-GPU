#pragma once
#include <cstdint>
#if defined(__CUDA_ARCH__)
namespace keyhunt::gpu::cuda_field {
// One statement owns each carry/borrow chain. Local PTX registers avoid early
// output clobbers when nvcc assigns the same register to an input and output.
// These helpers only add/subtract 256-bit words; field reduction stays shared.

__device__ __forceinline__ void add_words(uint32_t* out, const uint32_t* a,
                                                const uint32_t* b, uint32_t& carry) {
    asm("{\n\t"
        ".reg .u32 t<8>, c;\n\t"
        "add.cc.u32 t0, %9, %17;\n\t"
        "addc.cc.u32 t1, %10, %18;\n\t"
        "addc.cc.u32 t2, %11, %19;\n\t"
        "addc.cc.u32 t3, %12, %20;\n\t"
        "addc.cc.u32 t4, %13, %21;\n\t"
        "addc.cc.u32 t5, %14, %22;\n\t"
        "addc.cc.u32 t6, %15, %23;\n\t"
        "addc.cc.u32 t7, %16, %24;\n\t"
        "addc.u32 c, 0, 0;\n\t"
        "mov.u32 %0, t0;\n\t"
        "mov.u32 %1, t1;\n\t"
        "mov.u32 %2, t2;\n\t"
        "mov.u32 %3, t3;\n\t"
        "mov.u32 %4, t4;\n\t"
        "mov.u32 %5, t5;\n\t"
        "mov.u32 %6, t6;\n\t"
        "mov.u32 %7, t7;\n\t"
        "mov.u32 %8, c;\n\t"
        "}"
        : "=r"(out[0]), "=r"(out[1]), "=r"(out[2]), "=r"(out[3]),
          "=r"(out[4]), "=r"(out[5]), "=r"(out[6]), "=r"(out[7]), "=r"(carry)
        : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(a[4]), "r"(a[5]), "r"(a[6]), "r"(a[7]),
          "r"(b[0]), "r"(b[1]), "r"(b[2]), "r"(b[3]), "r"(b[4]), "r"(b[5]), "r"(b[6]), "r"(b[7]));
}

__device__ __forceinline__ void subtract_words(uint32_t* out, const uint32_t* a,
                                                const uint32_t* b, uint32_t& carry) {
    asm("{\n\t"
        ".reg .u32 t<8>, c;\n\t"
        "sub.cc.u32 t0, %9, %17;\n\t"
        "subc.cc.u32 t1, %10, %18;\n\t"
        "subc.cc.u32 t2, %11, %19;\n\t"
        "subc.cc.u32 t3, %12, %20;\n\t"
        "subc.cc.u32 t4, %13, %21;\n\t"
        "subc.cc.u32 t5, %14, %22;\n\t"
        "subc.cc.u32 t6, %15, %23;\n\t"
        "subc.cc.u32 t7, %16, %24;\n\t"
        "subc.u32 c, 0, 0;\n\t"
        // subc(0,0) produces 0xffffffff for a borrow; expose a 0/1 flag.
        "neg.s32 c, c;\n\t"
        "mov.u32 %0, t0;\n\t"
        "mov.u32 %1, t1;\n\t"
        "mov.u32 %2, t2;\n\t"
        "mov.u32 %3, t3;\n\t"
        "mov.u32 %4, t4;\n\t"
        "mov.u32 %5, t5;\n\t"
        "mov.u32 %6, t6;\n\t"
        "mov.u32 %7, t7;\n\t"
        "mov.u32 %8, c;\n\t"
        "}"
        : "=r"(out[0]), "=r"(out[1]), "=r"(out[2]), "=r"(out[3]),
          "=r"(out[4]), "=r"(out[5]), "=r"(out[6]), "=r"(out[7]), "=r"(carry)
        : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(a[4]), "r"(a[5]), "r"(a[6]), "r"(a[7]),
          "r"(b[0]), "r"(b[1]), "r"(b[2]), "r"(b[3]), "r"(b[4]), "r"(b[5]), "r"(b[6]), "r"(b[7]));
}
} // namespace keyhunt::gpu::cuda_field
#endif
