#pragma once
#include "device_runtime.h"
#include "common/point.h"
#include "keyhunt/core/bsgs_search.h"

namespace keyhunt::gpu {
struct BsgsCounters {
    unsigned long long steps=0,candidates=0,tail_rejections=0;
    unsigned overflow=0,invalid=0;
};
// CPU-generated cached operands are finite and have Z=1. CUDA uses its C18
// cached-point specialization. HIP group-8 retains the general formula because
// mixed addition regressed that workload (see docs/HIP_TUNING.md).
template<unsigned Group>
__device__ inline void bsgs_add_cached(Point& out,const Point& a,const Point& b) {
#if defined(__CUDACC__)
    point_add_cached(out,a,b);
#else
    if constexpr (Group==1) point_add_mixed(out,a,Affine{b.x,b.y,false});
    else point_add(out,a,b);
#endif
}
// One lane walks a consecutive group for one target. All absolute-start work
// is amortized in aG, while cached -(m*2^bit)G points seed each local giant index.
// Infinity is expected (j=0), including inside a batch inversion group.
template<unsigned Group>
// All launch sites use 128 threads. Exposing that bound lets gfx942 allocate
// registers for this block size instead of spilling for hypothetical larger blocks.
static __global__ __launch_bounds__(128) void bsgs_search(Point negative_start,uint64_t giants,uint64_t last_babies,
    const Point* targets,uint32_t first_target,const Point* powers,bsgs::View table,
    core::BsgsCandidate* output,uint32_t capacity,BsgsCounters* counters) {
    const uint64_t first=(uint64_t(blockIdx.x)*blockDim.x+threadIdx.x)*Group;
    if (first>=giants) return;
    const uint32_t target=first_target+blockIdx.y;
    const unsigned count=unsigned(giants-first<Group?giants-first:Group);
    Point current;
    bsgs_add_cached<Group>(current,targets[target],negative_start);
    for (unsigned bit=0;bit<20;++bit)
        if ((first>>bit)&1) bsgs_add_cached<Group>(current,current,powers[bit]);
    Field xs[Group],ys[Group],zs[Group];
    for (unsigned i=0;i<count;++i) {
        xs[i]=current.x; ys[i]=current.y; zs[i]=current.z;
        if (i+1<count) bsgs_add_cached<Group>(current,current,powers[0]);
    }
    if constexpr (Group==1) inverse(zs[0],zs[0]);
    else batch_inverse<Group>(zs,zs,count);
    for (unsigned i=0;i<count;++i) {
        bsgs::Key key{};
        if (!is_zero(zs[i])) {
            Field zz,x,y;
            square(zz,zs[i]); mul(x,xs[i],zz);
            mul(zz,zz,zs[i]); mul(y,ys[i],zz);
            key.bytes[0]=uint8_t(2+(y.limb[0]&1));
            to_bytes(key.bytes+1,x);
        }
        const auto hits=bsgs::lookup(table,key);
        for (auto entry=hits.begin;entry<hits.end;++entry) {
            const uint64_t j=table.entries[entry].j,giant=first+i;
            if (j>=table.count) { atomicExch(&counters->invalid,1U); continue; }
            if (giant==giants-1 && j>=last_babies) {
                atomicAdd(&counters->tail_rejections,1ULL); continue;
            }
            const auto slot=atomicAdd(&counters->candidates,1ULL);
            if (slot<capacity) output[slot]={giant,j,target,0};
            else atomicExch(&counters->overflow,1U);
        }
    }
    atomicAdd(&counters->steps,static_cast<unsigned long long>(count));
}
} // namespace keyhunt::gpu
