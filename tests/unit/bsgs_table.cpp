#include "keyhunt/core/bsgs_table.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace keyhunt::bsgs;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch(const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
int main() {
    try {
        rejects([]{plan(0);}); rejects([]{plan(UINT64_MAX);}); rejects([]{plan(17,{7});});
        const auto p=plan(257);
        require(p.buckets==64 && p.bloom_words==128,"geometry rounding failed");
        rejects([&]{plan(257,{16,p.host_peak_bytes-1});});
        require(plan(257,{16,p.host_peak_bytes}).host_peak_bytes==p.host_peak_bytes,"exact budget rejected");
        const auto wide=plan(1ULL<<32,{16,UINT64_MAX});
        require(wide.m==(1ULL<<32) && wide.resident_bytes>(1ULL<<32),"wide byte count truncated");
        require_device_memory(100,200,100);
        rejects([]{require_device_memory(101,200,100);}); rejects([]{require_device_memory(1,0,UINT64_MAX);});
        for (unsigned m : {1,2,7,8,9,31,127,128,129,257}) {
            const auto table=Table::build(m);
            for (const auto& e : table.entries()) {
                require(maybe_contains(table.view(),e.key),"Bloom false negative");
                const auto hit=lookup(table.view(),e.key), exact=lookup(table.view(),e.key,false);
                require(hit.end-hit.begin==1 && hit.begin==exact.begin && table.entries()[hit.begin].j==e.j,"table miss");
                if (!e.j) require(e.key.bytes[0]==0,"zero baby step missing");
                else {
                    auto negative=e.key; negative.bytes[0]^=1;
                    require(lookup(table.view(),negative).begin==lookup(table.view(),negative).end,"opposite sign conflated");
                }
            }
        }
        // One synthetic bucket deliberately contains identical keys with distinct
        // 64-bit indices, plus opposite Y parity and same-prefix low-byte variants.
        // A saturated Bloom filter forces every lookup through exact comparison.
        Entry entries[5]{};
        entries[0].j=0;
        for (unsigned i=1;i<5;++i) { entries[i].key.bytes[0]=2; entries[i].key.bytes[32]=1; entries[i].j=(1ULL<<40)+i; }
        entries[3].key.bytes[32]=2; entries[4].key.bytes[0]=3;
        uint64_t offsets[]{0,5}, filter=UINT64_MAX;
        View view{entries,offsets,&filter,5,1,1};
        const auto range=lookup(view,entries[1].key);
        require(range.begin==1 && range.end==3,"collision list lost entries");
        require(entries[range.begin].j>(1ULL<<32),"large baby index lost");
        require(lookup(view,entries[4].key).begin==4,"opposite sign missing");
        auto miss=entries[1].key; miss.bytes[32]=3;
        require(maybe_contains(view,miss) && lookup(view,miss).begin==lookup(view,miss).end,"Bloom false positive accepted");
        std::cout<<"BSGS geometry, memory, signs, infinity and complete collision lists passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
