#include "keyhunt/backend/gpu_bsgs_table.h"
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
        for (const char* operation : {"gpuMemGetInfo","gpuStreamCreateWithFlags","gpuEventCreate(start)",
            "gpuEventCreate(uploaded)","gpuEventCreate(kernel_done)","gpuEventCreate(done)","gpuMalloc(entries)",
            "gpuMalloc(offsets)","gpuMalloc(bloom)","gpuMalloc(keys)","gpuMalloc(hits)","gpuMalloc(count)",
            "gpuHostMalloc(keys)","gpuHostMalloc(hits)","gpuHostMalloc(count)","gpuEventRecord(prepare_start)",
            "gpuMemcpyAsync(entries)","gpuMemcpyAsync(offsets)","gpuMemcpyAsync(bloom)","gpuEventRecord(prepare_done)",
            "gpuEventSynchronize(prepare)","gpuEventElapsedTime(prepare)"}) {
            backend::injected_failure=operation;
            expect([&]{backend::GpuBsgsTable owner(0,table);},operation);
            ++tested;
        }
        for (const char* operation : {"gpuMemsetAsync(hits)","gpuMemsetAsync(count)","gpuEventRecord(start)",
            "gpuMemcpyAsync(keys)","gpuEventRecord(uploaded)","bsgs_probe launch","gpuEventRecord(kernel_done)",
            "gpuMemcpyAsync(hits)","gpuMemcpyAsync(count)","gpuEventRecord(done)","gpuEventSynchronize(probe)",
            "gpuEventElapsedTime(upload)","gpuEventElapsedTime(kernel)","gpuEventElapsedTime(download)"}) {
            backend::GpuBsgsTable owner(0,table);
            backend::injected_failure=operation;
            expect([&]{owner.probe(queries);},operation);
            expect([&]{owner.probe(queries);},"table failed");
            expect([&]{owner.device_view();},"table failed");
            ++tested;
        }
        for (const char* fault : {"count","guard","hit","filter"}) {
            backend::GpuBsgsTable owner(0,table);
            backend::bsgs_test_corruption=fault;
            expect([&]{owner.probe(queries);},"BSGS");
            expect([&]{owner.device_view();},"table failed");
            ++tested;
        }
        backend::GpuBsgsTable recovered(0,table);
        if (recovered.probe(queries).device_queries!=queries.size()) throw std::runtime_error("fresh upload did not recover");
        std::cout<<tested<<" GPU BSGS failure/corruption boundaries passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
