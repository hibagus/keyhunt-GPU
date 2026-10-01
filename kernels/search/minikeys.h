#pragma once
#include "device_runtime.h"
#include "common/minikey.h"
#include "keyhunt/core/xpoint_search.h"
namespace keyhunt::gpu {
struct MinikeysDeviceTarget{uint8_t bytes[22];};
struct MinikeysCounters{unsigned long long steps=0,candidates=0;unsigned overflow=0,invalid=0;};
__device__ inline int minikeys_compare(const uint8_t* a,const uint8_t* b){
    for(unsigned i=0;i<22;++i)if(a[i]!=b[i])return a[i]<b[i]?-1:1;return 0;
}
__global__ void minikeys_direct(Scalar begin,uint64_t count,unsigned length,uint8_t encodings,
    const MinikeysDeviceTarget* targets,uint32_t target_count,core::XPointCandidate* output,
    uint32_t capacity,MinikeysCounters* counters){
    const uint64_t offset=uint64_t(blockIdx.x)*blockDim.x+threadIdx.x;if(offset>=count)return;
    uint64_t carry=offset;
    for(unsigned i=0;i<8;++i){const uint64_t sum=uint64_t(begin.limb[i])+uint32_t(carry);
        begin.limb[i]=uint32_t(sum);carry=(carry>>32)+(sum>>32);}
    uint8_t text[31];Scalar scalar;
    if(carry||!minikey_text(begin,length,text)){atomicExch(&counters->invalid,1U);return;}
    // Rejected check bytes are tested ordinals too. The owner credits the exact
    // attempted interval only after all lanes finish and the output fits.
    if(minikey_private(text,length,scalar)){
        Point point;if(!public_key(point,scalar)||is_infinity(point)){atomicExch(&counters->invalid,1U);return;}
        Field zi,zz,zzz,x,y;
#if defined(__CUDACC__)
        inverse_binary(zi,point.z);
#else
        inverse(zi,point.z);
#endif
        square(zz,zi);mul(zzz,zz,zi);mul(x,point.x,zz);mul(y,point.y,zzz);
        uint8_t serialized[65],target[22];target[0]=uint8_t(length);to_bytes(serialized+1,x);
        for(uint8_t tag=1;tag<=2;++tag)if(encodings&tag){
            target[1]=tag;
            if(tag==1)serialized[0]=uint8_t(2+(y.limb[0]&1));
            else{serialized[0]=4;to_bytes(serialized+33,y);}
            hash::public_key(serialized,tag==1?33:65,target+2);
            uint32_t lo=0,hi=target_count;
            while(lo<hi){const uint32_t mid=lo+(hi-lo)/2;if(minikeys_compare(targets[mid].bytes,target)<0)lo=mid+1;else hi=mid;}
            if(lo==target_count||minikeys_compare(targets[lo].bytes,target))continue;
            const auto slot=atomicAdd(&counters->candidates,1ULL);
            if(slot<capacity)output[slot]={offset,lo,0};else atomicExch(&counters->overflow,1U);
        }
    }
    atomicAdd(&counters->steps,1ULL);
}
} // namespace keyhunt::gpu
