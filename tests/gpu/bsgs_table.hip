#include "keyhunt/backend/hip_bsgs_table.h"
#include "keyhunt/backend/device.h"
#include "runtime.h"
#include "bsgs_probe.h"
#include <algorithm>
#include <iostream>
#include <limits>
using namespace keyhunt;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch(const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
template<class T> struct Buffer {
    T* data=nullptr;
    explicit Buffer(size_t count) { backend::hip_check(hipMalloc(&data,count*sizeof(T)),"test allocation"); }
    ~Buffer() { if (data) (void)hipFree(data); }
};
int main() {
    try {
        require(!backend::discover_hip().devices.empty(),"hardware required");
        const auto table=bsgs::Table::build(257);
        rejects([&]{backend::HipBsgsTable invalid(-1,table);});
        rejects([&]{backend::BsgsUploadOptions o; o.max_queries=0; backend::HipBsgsTable invalid(0,table,o);});
        rejects([&]{backend::BsgsUploadOptions o; o.max_queries=65537; backend::HipBsgsTable invalid(0,table,o);});
        rejects([&]{backend::BsgsUploadOptions o; o.host_memory_bytes=1; backend::HipBsgsTable invalid(0,table,o);});
        rejects([&]{backend::BsgsUploadOptions o; o.memory_reserve_bytes=UINT64_MAX; backend::HipBsgsTable invalid(0,table,o);});
        backend::BsgsUploadOptions options; options.max_queries=257;
        backend::HipBsgsTable first(0,table,options), second(0,table,options);
        rejects([&]{first.probe({});}); rejects([&]{first.probe(std::vector<bsgs::Key>(258));});
        std::vector<bsgs::Key> keys;
        for (const auto& e:table.entries()) keys.push_back(e.key);
        const auto kept=first.probe(keys);
        for (unsigned count : {1,2,7,127,128,129,256,257}) {
            auto part=std::vector<bsgs::Key>(keys.begin(),keys.begin()+count);
            const auto a=first.probe(part),b=second.probe(part,false);
            require(a.device_queries==count && b.device_queries==count,"device count differs");
            for (unsigned i=0;i<count;++i) require(a.hits[i].j==b.hits[i].j && a.hits[i].j==table.entries()[i].j,"parity failure");
            for (auto& key:part) { key.bytes[0]=3; key.bytes[31]^=0x81; key.bytes[32]^=0x17; }
            (void)first.probe(part); (void)second.probe(part,false);
        }
        require(kept.hits[0].j==table.entries()[0].j,"owned result changed");
        require(first.device_view().count==257 && first.device_bytes()>table.memory().resident_bytes,"upload view missing");
        for (auto bits : {8U,32U}) {
            const auto alternative=bsgs::Table::build(129,{bits});
            backend::HipBsgsTable owner(0,alternative);
            std::vector<bsgs::Key> q;
            for (const auto& e:alternative.entries()) q.push_back(e.key);
            require(owner.probe(q).device_queries==129,"filter policy failed");
        }
        // Raw-layout test only: force duplicate keys, opposite Y, a saturated
        // Bloom filter and indices above 32 bits through the actual device kernel.
        bsgs::Entry entries[5]{};
        for (unsigned i=1;i<5;++i) { entries[i].key.bytes[0]=2; entries[i].key.bytes[32]=1; entries[i].j=(1ULL<<40)+i; }
        entries[3].key.bytes[32]=2; entries[4].key.bytes[0]=3;
        uint64_t offsets[]{0,5},bloom=UINT64_MAX;
        bsgs::Key queries[]{entries[0].key,entries[1].key,entries[4].key,entries[3].key}; queries[3].bytes[32]=3;
        Buffer<bsgs::Entry> de(5); Buffer<uint64_t> offsets_device(2),filter(1);
        Buffer<bsgs::Key> dq(4); Buffer<backend::BsgsProbeHit> out(4); Buffer<unsigned long long> counter(1);
        using backend::hip_check;
        hip_check(hipMemcpy(de.data,entries,sizeof(entries),hipMemcpyHostToDevice),"test entries");
        hip_check(hipMemcpy(offsets_device.data,offsets,sizeof(offsets),hipMemcpyHostToDevice),"test offsets");
        hip_check(hipMemcpy(filter.data,&bloom,sizeof(bloom),hipMemcpyHostToDevice),"test filter");
        hip_check(hipMemcpy(dq.data,queries,sizeof(queries),hipMemcpyHostToDevice),"test keys");
        hip_check(hipMemset(counter.data,0,sizeof(*counter.data)),"test count");
        bsgs::View view{de.data,offsets_device.data,filter.data,5,1,1};
        (void)hipGetLastError();
        hipLaunchKernelGGL(gpu::bsgs_probe,dim3(1),dim3(128),0,0,view,dq.data,4,out.data,counter.data,true);
        hip_check(hipGetLastError(),"synthetic collision launch");
        backend::BsgsProbeHit hits[4]; unsigned long long count=0;
        hip_check(hipMemcpy(hits,out.data,sizeof(hits),hipMemcpyDeviceToHost),"test hits");
        hip_check(hipMemcpy(&count,counter.data,sizeof(count),hipMemcpyDeviceToHost),"test count result");
        require(count==4 && hits[1].begin==1 && hits[1].end==3 && hits[1].j==(1ULL<<40)+1,"collision list truncated");
        require(hits[0].j==0 && hits[2].j==(1ULL<<40)+4,"zero/sign identity wrong");
        require(hits[3].bloom_positive && hits[3].begin==hits[3].end,"false positive accepted");
        std::cout<<"HIP BSGS upload, tails, filter parity, signs and full collision lists passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
