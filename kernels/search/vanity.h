#pragma once
#include "common/scalar_stride.h"
#include "device_runtime.h"
#include "common/point.h"
#include "common/base58check.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::gpu {
#if defined(__CUDACC__)
using VanityPower=Point;
#define KEYHUNT_vanity_LAUNCH_BOUND
#else
using VanityPower=Affine;
#define KEYHUNT_vanity_LAUNCH_BOUND __launch_bounds__(128)
#endif
struct VanityCounters {
    unsigned long long steps=0,candidates=0;
    unsigned overflow=0,invalid=0;
};
// A plain byte array avoids calling a host-only std::array implementation from
// device code. The host uploads sorted 36-byte encoding/prefix relations directly.
struct VanityDeviceTarget { uint8_t bytes[36]; };
__device__ inline void vanity_add_cached(Point& out,const Point& a,const VanityPower& b) {
#if defined(__CUDACC__)
    point_add_cached(out,a,b);
#else
    point_add_mixed(out,a,b);
#endif
}
__device__ inline int vanity_compare(const uint8_t* a,const uint8_t* b) {
    for(unsigned i=0;i<36;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;
    return 0;
}
__device__ inline void vanity_lookup(const Field& x,const Field& y,uint64_t offset,
    uint64_t compressed_lengths,uint64_t uncompressed_lengths,
    const VanityDeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,VanityCounters* counters) {
    uint8_t serialized[65],digest[20];to_bytes(serialized+1,x);
    for(uint8_t tag=1;tag<=2;++tag) {
        const uint64_t lengths=tag==1?compressed_lengths:uncompressed_lengths;
        if(!lengths)continue;
        if(tag==1)serialized[0]=uint8_t(2+(y.limb[0]&1));
        else {serialized[0]=4;to_bytes(serialized+33,y);}
        hash::public_key(serialized,tag==1?33:65,digest);
        char address[35];const unsigned size=hash::p2pkh_address(digest,address);
        if(!size){atomicExch(&counters->invalid,1U);return;}
        uint8_t target[36]{};target[0]=tag;
        // Extending the zero-padded key checks every requested prefix length.
        // Different lengths can match simultaneously and must all be emitted.
        for(unsigned length=1;length<=size;++length) {
            target[1]=uint8_t(length);target[length+1]=uint8_t(address[length-1]);
            if(!(lengths&(uint64_t(1)<<length)))continue;
            uint32_t lo=0,hi=target_count;
            while(lo<hi){const uint32_t mid=lo+(hi-lo)/2;
                if(vanity_compare(targets[mid].bytes,target)<0)lo=mid+1;else hi=mid;}
            if(lo==target_count||vanity_compare(targets[lo].bytes,target))continue;
            const auto slot=atomicAdd(&counters->candidates,1ULL);
            if(slot<capacity)output[slot]={offset,lo,0};else atomicExch(&counters->overflow,1U);
        }
    }
}
__device__ inline Scalar vanity_offset_scalar(Scalar begin,uint64_t offset) {
    // The host proves the exact nonwrapping interval lies in [1,n).
    uint64_t carry=offset;
    for(unsigned i=0;i<8;++i){const uint64_t sum=uint64_t(begin.limb[i])+uint32_t(carry);
        begin.limb[i]=uint32_t(sum);carry=(carry>>32)+(sum>>32);}
    return begin;
}
__device__ inline void vanity_normalized_lookup(const Field& px,const Field& py,const Field& zi,
    uint64_t offset,uint64_t compressed_lengths,uint64_t uncompressed_lengths,const VanityDeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,VanityCounters* counters) {
    Field zz,zzz,x,y;square(zz,zi);mul(zzz,zz,zi);mul(x,px,zz);mul(y,py,zzz);
    vanity_lookup(x,y,offset,compressed_lengths,uncompressed_lengths,targets,target_count,output,capacity,counters);
}
template<bool Strided>
__global__ KEYHUNT_vanity_LAUNCH_BOUND void vanity_direct(Scalar begin,Scalar stride,uint64_t count,uint64_t compressed_lengths,uint64_t uncompressed_lengths,
    const VanityDeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,VanityCounters* counters) {
    const uint64_t index=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;
    if(index>=count)return;
    Scalar scalar;
    if constexpr(Strided){
        if(!stride_scalar(begin,stride,index,scalar)){atomicExch(&counters->invalid,1U);return;}
    }else scalar=vanity_offset_scalar(begin,index);
    Point point;
    if(!public_key(point,scalar)||is_infinity(point)){
        atomicExch(&counters->invalid,1U);return;}
    Field zi;
#if defined(__CUDACC__)
    inverse_binary(zi,point.z);
#else
    inverse(zi,point.z);
#endif
    vanity_normalized_lookup(point.x,point.y,zi,index,compressed_lengths,uncompressed_lengths,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,1ULL);
}
// Four consecutive scalars share one inversion. Both coordinates must be kept:
// hashing compressed keys still needs affine Y parity. Partial groups execute
// exactly the remaining scalars, with no modular wrap or padded credited work.
constexpr unsigned vanity_group=4;
__global__ KEYHUNT_vanity_LAUNCH_BOUND void vanity_stepped(Point base,uint64_t count,
    const VanityPower* powers,uint64_t compressed_lengths,uint64_t uncompressed_lengths,const VanityDeviceTarget* targets,uint32_t target_count,
    core::XPointCandidate* output,uint32_t capacity,VanityCounters* counters) {
    const uint64_t first=(uint64_t(blockIdx.x)*blockDim.x+threadIdx.x)*vanity_group;
    if(first>=count)return;
    const unsigned steps=unsigned(count-first<vanity_group?count-first:vanity_group);
    Point current=base;
    for(unsigned bit=0;bit<20;++bit)if((first>>bit)&1)vanity_add_cached(current,current,powers[bit]);
    Field xs[vanity_group],ys[vanity_group],zs[vanity_group];
    for(unsigned i=0;i<steps;++i){
        if(is_infinity(current)){atomicExch(&counters->invalid,1U);return;}
        xs[i]=current.x;ys[i]=current.y;zs[i]=current.z;
        if(i+1<steps)vanity_add_cached(current,current,powers[0]);
    }
    batch_inverse<vanity_group>(zs,zs,steps);
    for(unsigned i=0;i<steps;++i)
        vanity_normalized_lookup(xs[i],ys[i],zs[i],first+i,compressed_lengths,uncompressed_lengths,targets,target_count,output,capacity,counters);
    atomicAdd(&counters->steps,static_cast<unsigned long long>(steps));
}
#undef KEYHUNT_vanity_LAUNCH_BOUND
} // namespace keyhunt::gpu
