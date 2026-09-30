#pragma once
#include <hip/hip_runtime.h>
#include "common/point.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::gpu {
struct XPointCounters {
    unsigned long long steps = 0, candidates = 0;
    unsigned overflow = 0, invalid = 0;
};
__device__ inline void xpoint_emit(uint64_t offset, uint32_t target,
    core::XPointCandidate* output, uint32_t capacity, XPointCounters* counters) {
    const auto slot = atomicAdd(&counters->candidates,1ULL);
    if (slot < capacity) output[slot] = {offset,target,0};
    else atomicExch(&counters->overflow,1U); // never write beyond the bounded slot
}
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
    xpoint_emit(offset,lo,output,capacity,counters);
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
// Every host launch uses 128 threads. Tell the compiler that bound so it can
// allocate registers for this block size instead of spilling for larger blocks.
__global__ __launch_bounds__(128) void xpoint_direct(Scalar begin, uint64_t count, const Field* targets,
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
// Each lane owns a consecutive eight-scalar interval. Starting from begin*G
// computed once by the host, add at most 20 cached powers of G for the local
// offset, then step by G. This avoids repeating a 256-bit multiply per scalar.
// Disjoint lane groups cover [0,count) exactly, including the final partial group.
constexpr unsigned xpoint_group = 8;
template<bool SmallTargets>
__global__ __launch_bounds__(128) void xpoint_stepped(Point base, uint64_t count, const Affine* powers,
    const Field* targets, uint32_t target_count, core::XPointCandidate* output,
    uint32_t capacity, XPointCounters* counters) {
    const uint64_t first = (uint64_t(blockIdx.x)*blockDim.x+threadIdx.x)*xpoint_group;
    if (first >= count) return;
    const unsigned steps = unsigned(count-first < xpoint_group ? count-first : xpoint_group);
    Point current = base;
    for (unsigned bit=0;bit<20;++bit)
        if ((first >> bit)&1) point_add_mixed(current,current,powers[bit]);
    Field xs[xpoint_group], zs[xpoint_group];
    for (unsigned i=0;i<steps;++i) {
        if (is_infinity(current)) { atomicExch(&counters->invalid,1U); return; }
        if constexpr (SmallTargets) {
            // For up to four targets, compare X == target_x * Z^2 directly.
            // Z is nonzero, so this is exactly affine-X equality without inversion.
            Field zz, target_x;
            square(zz,current.z);
            for (uint32_t t=0;t<target_count;++t) {
                mul(target_x,targets[t],zz);
                if (equal(current.x,target_x)) xpoint_emit(first+i,t,output,capacity,counters);
            }
        } else {
            xs[i] = current.x; zs[i] = current.z;
        }
        if (i+1 < steps) point_add_mixed(current,current,powers[0]);
    }
    if constexpr (!SmallTargets) {
        // One Fermat inversion for the whole lane group. Only X and Z are kept;
        // no per-point Y normalization or unbounded scratch allocation is needed.
        batch_inverse<xpoint_group>(zs,zs,steps);
        for (unsigned i=0;i<steps;++i) {
            Field zz,x;
            square(zz,zs[i]); mul(x,xs[i],zz);
            xpoint_lookup(x,first+i,targets,target_count,output,capacity,counters);
        }
    }
    atomicAdd(&counters->steps,static_cast<unsigned long long>(steps));
}
} // namespace keyhunt::gpu
