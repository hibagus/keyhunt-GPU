#pragma once
#include "device_runtime.h"
#include "common/point.h"
#include "common/keccak.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::gpu {
#if defined(__CUDACC__)
using EthereumPower=Point;
#define KEYHUNT_ETHEREUM_LAUNCH_BOUND
#else
using EthereumPower=Affine;
#define KEYHUNT_ETHEREUM_LAUNCH_BOUND __launch_bounds__(128)
#endif
struct EthereumCounters {
    unsigned long long steps=0,candidates=0;
    unsigned overflow=0,invalid=0;
};
// A plain byte array avoids calling a host-only std::array implementation from
// device code. The host uploads sorted 20-byte addresses directly.
struct EthereumDeviceTarget { uint8_t bytes[20]; };
__device__ inline void ethereum_add_cached(Point& out,const Point& a,const EthereumPower& b) {
#if defined(__CUDACC__)
    point_add_cached(out,a,b);
#else
    point_add_mixed(out,a,b);
#endif
}
__device__ inline int ethereum_compare(const uint8_t* a,const uint8_t* b) {
    for(unsigned i=0;i<20;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    return 0;
}
__device__ inline void ethereum_lookup(const Field& x,const Field& y,uint64_t offset,
    const EthereumDeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,EthereumCounters* counters) {
    // SEC1's 0x04 prefix is excluded. Both affine coordinates are big-endian;
    // the sponge handles little-endian lane absorption internally.
    uint8_t serialized[64],digest[32];
    to_bytes(serialized,x);to_bytes(serialized+32,y);
    hash::keccak256_short(serialized,64,digest);
    const auto* target=digest+12;
    uint32_t lo=0,hi=target_count;
    while(lo<hi) {
        const uint32_t mid=lo+(hi-lo)/2;
        if(ethereum_compare(targets[mid].bytes,target)<0)lo=mid+1;else hi=mid;
    }
    if(lo==target_count || ethereum_compare(targets[lo].bytes,target))return;
    const auto slot=atomicAdd(&counters->candidates,1ULL);
    if(slot<capacity)output[slot]={offset,lo,0};
    else atomicExch(&counters->overflow,1U);
}
__device__ inline Scalar ethereum_offset_scalar(Scalar begin,uint64_t offset) {
    // The host proves the exact nonwrapping interval lies in [1,n).
    uint64_t carry=offset;
    for(unsigned i=0;i<8;++i){const uint64_t sum=uint64_t(begin.limb[i])+uint32_t(carry);
        begin.limb[i]=uint32_t(sum);carry=(carry>>32)+(sum>>32);}
    return begin;
}
__device__ inline void ethereum_normalized_lookup(const Field& px,const Field& py,const Field& zi,
    uint64_t offset,const EthereumDeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,EthereumCounters* counters) {
    Field zz,zzz,x,y;square(zz,zi);mul(zzz,zz,zi);mul(x,px,zz);mul(y,py,zzz);
    ethereum_lookup(x,y,offset,targets,target_count,output,capacity,counters);
}
__global__ KEYHUNT_ETHEREUM_LAUNCH_BOUND void ethereum_direct(Scalar begin,uint64_t count,
    const EthereumDeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,EthereumCounters* counters) {
    const uint64_t index=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(index>=count)return;
    Point point;
    if(!public_key(point,ethereum_offset_scalar(begin,index))||is_infinity(point)){
        atomicExch(&counters->invalid,1U);return;}
    Field zi;
#if defined(__CUDACC__)
    inverse_binary(zi,point.z);
#else
    inverse(zi,point.z);
#endif
    ethereum_normalized_lookup(point.x,point.y,zi,index,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,1ULL);
}
// Four consecutive scalars share one inversion. Both coordinates must be kept:
// Ethereum hashes both full coordinates. Partial groups execute
// exactly the remaining scalars, with no modular wrap or padded credited work.
constexpr unsigned ethereum_group=4;
__global__ KEYHUNT_ETHEREUM_LAUNCH_BOUND void ethereum_stepped(Point base,uint64_t count,
    const EthereumPower* powers,const EthereumDeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,EthereumCounters* counters) {
    const uint64_t first=(uint64_t(blockIdx.x)*blockDim.x+threadIdx.x)*ethereum_group;
    if(first>=count)return;
    const unsigned steps=unsigned(count-first<ethereum_group?count-first:ethereum_group);
    Point current=base;
    for(unsigned bit=0;bit<20;++bit)if((first>>bit)&1)ethereum_add_cached(current,current,powers[bit]);
    Field xs[ethereum_group],ys[ethereum_group],zs[ethereum_group];
    for(unsigned i=0;i<steps;++i){
        if(is_infinity(current)){atomicExch(&counters->invalid,1U);return;}
        xs[i]=current.x;ys[i]=current.y;zs[i]=current.z;
        if(i+1<steps)ethereum_add_cached(current,current,powers[0]);
    }
    batch_inverse<ethereum_group>(zs,zs,steps);
    for(unsigned i=0;i<steps;++i)
        ethereum_normalized_lookup(xs[i],ys[i],zs[i],first+i,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,static_cast<unsigned long long>(steps));
}
#undef KEYHUNT_ETHEREUM_LAUNCH_BOUND
} // namespace keyhunt::gpu
