#include "keyhunt/backend/gpu_minikeys.h"
#include "runtime.h"
#include <algorithm>
#include <iostream>
using namespace keyhunt;
using core::UInt256;
template<class F> void expect_error(F fn,const std::string& text) {
    try { fn(); } catch(const std::exception& e) {
        if (std::string(e.what()).find(text)!=std::string::npos) return;
        throw;
    }
    throw std::runtime_error("missing expected error: "+text);
}
int main() {
    try {
        core::XPointVerifier verifier;
        const auto begin=core::minikey_ordinal("SzavMBLoXU6kDrqtUVmffv");
        const auto scalar=core::minikey_scalar("SzavMBLoXU6kDrqtUVmffv");
        core::MinikeyTargets targets({core::minikey_target(22,core::hash160_target(verifier.derive(*scalar),1))});
        scheduler::ExecutionIdentity id; id.target_digest=targets.digest();
        id.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;
        id.assignment_id[0]=1; id.assignment_generation=id.executor_generation=1;
        scheduler::BlockGrid grid(core::ScalarInterval(begin,begin.add(UInt256(2))),UInt256(2));
        auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),begin,2,id);
        auto batch=*scheduler::KernelBatch::plan(work,begin,2);
        unsigned tested=0;
        for (const char* operation : {"gpuStreamCreateWithFlags","gpuEventCreate(start)","gpuEventCreate(kernel_done)",
            "gpuEventCreate(done)","gpuMalloc(targets)","gpuMalloc(output)","gpuMalloc(count)",
            "gpuHostMalloc(output)","gpuHostMalloc(count)","gpuMemcpy(targets)"}) {
            backend::injected_failure=operation;
            expect_error([&] { backend::GpuMinikeysExecutor e(0,targets,verifier); },operation);
            ++tested;
        }
        for (const char* operation : {"gpuMemsetAsync(output)","gpuMemsetAsync(count)","gpuEventRecord(start)",
            "minikeys launch","gpuEventRecord(kernel_done)","gpuMemcpyAsync(output)","gpuMemcpyAsync(count)",
            "gpuEventRecord(done)","gpuEventQuery","gpuEventElapsedTime(kernel)","gpuEventElapsedTime(download)",
            "gpuStreamSynchronize"}) {
            backend::GpuMinikeysExecutor e(0,targets,verifier);
            backend::injected_failure=operation;
            expect_error([&] { const auto t=e.submit(batch); e.drain(); (void)e.take(t); },operation);
            expect_error([&] { e.submit(batch); },"executor failed");
            expect_error([&] { e.drain(); },"executor failed");
            ++tested;
        }
        // Corrupt only downloaded test data, never running hardware or the device.
        // Each failure must poison the owner and cannot produce a coverage receipt.
        for (const auto& fault : {std::pair{"steps","counters"},{"count","counters"},{"overflow","counters"},
            {"guard","guard"},{"offset","record"},{"target","record"},{"false_match","GPU minikey candidate failed"}}) {
            backend::GpuMinikeysExecutor e(0,targets,verifier);
            auto ticket=e.submit(batch); e.drain();
            backend::minikeys_test_corruption=fault.first;
            expect_error([&] { (void)e.take(ticket); },fault.second);
            expect_error([&] { e.submit(batch); },"executor failed");
            ++tested;
        }
        backend::GpuMinikeysExecutor recovered(0,targets,verifier);
        auto ticket=recovered.submit(batch); recovered.drain();
        const auto result=recovered.take(ticket);
        if (result.verified_steps!=2 || result.matches.size()!=1) throw std::runtime_error("recovery failed");
        std::cout<<tested<<" GPU minikeys failure boundaries passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
