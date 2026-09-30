#pragma once
#include <hip/hip_runtime.h>
#include "keyhunt/backend/hip_bsgs_table.h"
namespace keyhunt::gpu {
static __global__ void bsgs_probe(bsgs::View table,const bsgs::Key* keys,uint64_t count,
    backend::BsgsProbeHit* hits,unsigned long long* executed,bool use_filter) {
    const uint64_t index=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if (index>=count) return;
    const auto range=bsgs::lookup(table,keys[index],use_filter);
    hits[index]={range.begin,range.end,range.begin<range.end ? table.entries[range.begin].j : UINT64_MAX,
        uint32_t(bsgs::maybe_contains(table,keys[index])),0};
    atomicAdd(executed,1ULL);
}
} // namespace keyhunt::gpu
