#pragma once
#include <hip/hip_runtime.h>
#include "common/point.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::gpu {
struct XPointCounters {
    unsigned long long steps = 0, candidates = 0;
    unsigned overflow = 0, invalid = 0;
};
// Targets are sorted by their full 256-bit value; no truncated fingerprints or
// Bloom positives can become a match. The host already removed duplicate Xs.
__device__ inline void xpoint_lookup(const Field& x, uint64_t offset,
    const Field* targets, uint32_t target_count, core::XPointCandidate* output,
    uint32_t capacity, XPointCounters* counters) {
    uint32_t lo = 0, hi = target_count;
    while (lo < hi) {
        const uint32_t mid = lo+(hi-lo)/2;
        if (less(targets[mid],x)) lo = mid+1; else hi = mid;
    }
    if (lo == target_count || !equal(targets[lo],x)) return;
    const auto slot = atomicAdd(&counters->candidates,1ULL);
    if (slot < capacity) output[slot] = {offset,lo,0};
    else atomicExch(&counters->overflow,1U); // never write beyond the bounded slot
}
__device__ inline Scalar offset_scalar(Scalar begin, uint64_t offset) {
    // The checked host interval proves this sum is < n, without modular wrap.
    uint64_t carry = offset;
    for (unsigned limb=0;limb<8;++limb) {
        const uint64_t sum = uint64_t(begin.limb[limb])+uint32_t(carry);
        begin.limb[limb] = uint32_t(sum);
        carry = (carry >> 32)+(sum >> 32);
    }
    return begin;
}
__global__ void xpoint_direct(Scalar begin, uint64_t count, const Field* targets,
    uint32_t target_count, core::XPointCandidate* output, uint32_t capacity,
    XPointCounters* counters) {
    const uint64_t index = uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if (index >= count) return;
    Point point;
    if (!public_key(point,offset_scalar(begin,index)) || is_infinity(point)) {
        atomicExch(&counters->invalid,1U);
        return;
    }
    // Only X is needed: avoid the extra multiplication to normalize Y.
    Field zi,zz,x;
    inverse(zi,point.z); square(zz,zi); mul(x,point.x,zz);
    xpoint_lookup(x,index,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,1ULL);
}
} // namespace keyhunt::gpu
