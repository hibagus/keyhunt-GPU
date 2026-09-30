#include "keyhunt/backend/hip_bsgs_table.h"
#include "runtime.h"
#include <iostream>
using namespace keyhunt;
template<class F> void expect(F fn,const std::string& text) {
    try { fn(); } catch(const std::exception& e) { if (std::string(e.what()).find(text)!=std::string::npos) return; throw; }
    throw std::runtime_error("expected failure: "+text);
}
int main() {
    try {
        const auto table=bsgs::Table::build(9);
        std::vector<bsgs::Key> queries;
        for (const auto& e:table.entries()) queries.push_back(e.key);
        unsigned tested=0;
        for (const char* operation : {"hipMemGetInfo","hipStreamCreateWithFlags","hipEventCreate(start)",
            "hipEventCreate(uploaded)","hipEventCreate(kernel_done)","hipEventCreate(done)","hipMalloc(entries)",
            "hipMalloc(offsets)","hipMalloc(bloom)","hipMalloc(keys)","hipMalloc(hits)","hipMalloc(count)",
            "hipHostMalloc(keys)","hipHostMalloc(hits)","hipHostMalloc(count)","hipEventRecord(prepare_start)",
            "hipMemcpyAsync(entries)","hipMemcpyAsync(offsets)","hipMemcpyAsync(bloom)","hipEventRecord(prepare_done)",
            "hipEventSynchronize(prepare)","hipEventElapsedTime(prepare)"}) {
            backend::injected_failure=operation;
            expect([&]{backend::HipBsgsTable owner(0,table);},operation);
            ++tested;
        }
        for (const char* operation : {"hipMemsetAsync(hits)","hipMemsetAsync(count)","hipEventRecord(start)",
            "hipMemcpyAsync(keys)","hipEventRecord(uploaded)","bsgs_probe launch","hipEventRecord(kernel_done)",
            "hipMemcpyAsync(hits)","hipMemcpyAsync(count)","hipEventRecord(done)","hipEventSynchronize(probe)",
            "hipEventElapsedTime(upload)","hipEventElapsedTime(kernel)","hipEventElapsedTime(download)"}) {
            backend::HipBsgsTable owner(0,table);
            backend::injected_failure=operation;
            expect([&]{owner.probe(queries);},operation);
            expect([&]{owner.probe(queries);},"table failed");
            expect([&]{owner.device_view();},"table failed");
            ++tested;
        }
        for (const char* fault : {"count","guard","hit","filter"}) {
            backend::HipBsgsTable owner(0,table);
            backend::bsgs_test_corruption=fault;
            expect([&]{owner.probe(queries);},"BSGS");
            expect([&]{owner.device_view();},"table failed");
            ++tested;
        }
        backend::HipBsgsTable recovered(0,table);
        if (recovered.probe(queries).device_queries!=queries.size()) throw std::runtime_error("fresh upload did not recover");
        std::cout<<tested<<" HIP BSGS failure/corruption boundaries passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
