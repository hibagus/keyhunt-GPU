#pragma once
#include "point.h"
namespace keyhunt::gpu {
// Kernel-local indices are bounded to at most 2^20 candidates. Multiplying one
// 32-bit limb by a 32-bit offset plus carry and base fits exactly in uint64_t.
// Reject overflow/order violations instead of turning them into modular search.
KEYHUNT_HD inline bool stride_scalar(Scalar begin,const Scalar& stride,uint64_t offset,Scalar& result) {
    if(offset>0xffffffffULL || !valid_scalar(stride))return false;
    uint64_t carry=0;
    for(unsigned limb=0;limb<8;++limb){
        const uint64_t product=uint64_t(stride.limb[limb])*uint32_t(offset)+begin.limb[limb]+carry;
        result.limb[limb]=uint32_t(product);carry=product>>32;
    }
    return carry==0 && valid_scalar(result);
}
} // namespace keyhunt::gpu
