#pragma once
#include "device_runtime.h"
#include "common/point.h"
#include "common/hash160.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::gpu {
#if defined(__CUDACC__)
using Hash160Power=Point;
#define KEYHUNT_HASH160_LAUNCH_BOUND
#else
using Hash160Power=Affine;
#define KEYHUNT_HASH160_LAUNCH_BOUND __launch_bounds__(128)
#endif
struct Hash160Counters {
    unsigned long long steps=0,candidates=0;
    unsigned overflow=0,invalid=0;
};
// A plain byte array avoids calling a host-only std::array implementation from
// device code. The host uploads sorted 21-byte encoding/hash relations directly.
struct Hash160DeviceTarget { uint8_t bytes[21]; };
__device__ inline void hash160_add_cached(Point& out,const Point& a,const Hash160Power& b) {
#if defined(__CUDACC__)
    point_add_cached(out,a,b);
#else
    point_add_mixed(out,a,b);
#endif
}
__device__ inline int hash160_compare(const uint8_t* a,const uint8_t* b) {
    for(unsigned i=0;i<21;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    return 0;
}
__device__ inline void hash160_lookup(const Field& x,const Field& y,uint64_t offset,uint8_t encodings,
    const Hash160DeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,Hash160Counters* counters) {
    uint8_t serialized[65],target[21];to_bytes(serialized+1,x);
    for(uint8_t tag=1;tag<=2;++tag)if(encodings&tag) {
        target[0]=tag;
        if(tag==1)serialized[0]=uint8_t(2+(y.limb[0]&1));
        else {serialized[0]=4;to_bytes(serialized+33,y);}
        hash::public_key(serialized,tag==1?33:65,target+1);
        uint32_t lo=0,hi=target_count;
        while(lo<hi){const uint32_t mid=lo+(hi-lo)/2;
            if(hash160_compare(targets[mid].bytes,target)<0)lo=mid+1;else hi=mid;}
        if(lo==target_count||hash160_compare(targets[lo].bytes,target))continue;
        const auto slot=atomicAdd(&counters->candidates,1ULL);
        if(slot<capacity)output[slot]={offset,lo,0};
        else atomicExch(&counters->overflow,1U);
    }
}
__device__ inline Scalar hash160_offset_scalar(Scalar begin,uint64_t offset) {
    // The host proves the exact nonwrapping interval lies in [1,n).
    uint64_t carry=offset;
    for(unsigned i=0;i<8;++i){const uint64_t sum=uint64_t(begin.limb[i])+uint32_t(carry);
        begin.limb[i]=uint32_t(sum);carry=(carry>>32)+(sum>>32);}
    return begin;
}
__device__ inline void hash160_normalized_lookup(const Field& px,const Field& py,const Field& zi,
    uint64_t offset,uint8_t encodings,const Hash160DeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,Hash160Counters* counters) {
    Field zz,zzz,x,y;square(zz,zi);mul(zzz,zz,zi);mul(x,px,zz);mul(y,py,zzz);
    hash160_lookup(x,y,offset,encodings,targets,target_count,output,capacity,counters);
}
__global__ KEYHUNT_HASH160_LAUNCH_BOUND void hash160_direct(Scalar begin,uint64_t count,uint8_t encodings,
    const Hash160DeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,Hash160Counters* counters) {
    const uint64_t index=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(index>=count)return;
    Point point;
    if(!public_key(point,hash160_offset_scalar(begin,index))||is_infinity(point)){
        atomicExch(&counters->invalid,1U);return;}
    Field zi;
#if defined(__CUDACC__)
    inverse_binary(zi,point.z);
#else
    inverse(zi,point.z);
#endif
    hash160_normalized_lookup(point.x,point.y,zi,index,encodings,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,1ULL);
}
// Four consecutive scalars share one inversion. Both coordinates must be kept:
// hashing compressed keys still needs affine Y parity. Partial groups execute
// exactly the remaining scalars, with no modular wrap or padded credited work.
constexpr unsigned hash160_group=4;
__global__ KEYHUNT_HASH160_LAUNCH_BOUND void hash160_stepped(Point base,uint64_t count,
    const Hash160Power* powers,uint8_t encodings,const Hash160DeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,Hash160Counters* counters) {
    const uint64_t first=(uint64_t(blockIdx.x)*blockDim.x+threadIdx.x)*hash160_group;
    if(first>=count)return;
    const unsigned steps=unsigned(count-first<hash160_group?count-first:hash160_group);
    Point current=base;
    for(unsigned bit=0;bit<20;++bit)if((first>>bit)&1)hash160_add_cached(current,current,powers[bit]);
    Field xs[hash160_group],ys[hash160_group],zs[hash160_group];
    for(unsigned i=0;i<steps;++i){
        if(is_infinity(current)){atomicExch(&counters->invalid,1U);return;}
        xs[i]=current.x;ys[i]=current.y;zs[i]=current.z;
        if(i+1<steps)hash160_add_cached(current,current,powers[0]);
    }
    batch_inverse<hash160_group>(zs,zs,steps);
    for(unsigned i=0;i<steps;++i)
        hash160_normalized_lookup(xs[i],ys[i],zs[i],first+i,encodings,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,static_cast<unsigned long long>(steps));
}
#undef KEYHUNT_HASH160_LAUNCH_BOUND
} // namespace keyhunt::gpu
