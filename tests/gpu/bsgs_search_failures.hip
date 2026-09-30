#include "keyhunt/backend/hip_bsgs.h"
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
        const auto pub=verifier.derive(UInt256(1));
        core::BsgsTargets targets({pub});
        const auto table=bsgs::Table::build(7);
        core::BsgsBatch batch(core::ScalarInterval(UInt256(1),UInt256(5)),7,0,1,targets.digest(),table.checksum());
        unsigned tested=0;
        for (const char* operation : {"bsgs hipStreamCreateWithFlags","bsgs hipEventCreate(start)","bsgs hipEventCreate(kernel_done)",
            "bsgs hipEventCreate(done)","bsgs hipMalloc(targets)","bsgs hipMalloc(output)","bsgs hipMalloc(count)",
            "bsgs hipHostMalloc(output)","bsgs hipHostMalloc(count)","bsgs hipMemcpy(targets)","bsgs hipMalloc(powers)","bsgs hipMemcpy(powers)","bsgs hipMemGetInfo(search)","bsgs hipDeviceGetAttribute(CUs)"}) {
            backend::injected_failure=operation;
            expect_error([&] { backend::HipBsgsExecutor e(0,table,targets,verifier); },operation);
            ++tested;
        }
        for (const char* operation : {"bsgs hipMemsetAsync(output)","bsgs hipMemsetAsync(count)","bsgs hipEventRecord(start)",
            "BSGS launch","bsgs hipEventRecord(kernel_done)","bsgs hipMemcpyAsync(output)","bsgs hipMemcpyAsync(count)",
            "bsgs hipEventRecord(done)","bsgs hipEventQuery","bsgs hipEventElapsedTime(kernel)","bsgs hipEventElapsedTime(download)",
            "bsgs hipStreamSynchronize"}) {
            backend::HipBsgsExecutor e(0,table,targets,verifier);
            backend::injected_failure=operation;
            expect_error([&] { const auto t=e.submit(batch); e.drain(); (void)e.take(t); },operation);
            expect_error([&] { e.submit(batch); },"executor failed");
            expect_error([&] { e.drain(); },"executor failed");
            ++tested;
        }
        // Corrupt only downloaded test data, never running hardware or the device.
        // Each failure must poison the owner and cannot produce a coverage receipt.
        for (const auto& fault : {std::pair{"steps","counters"},{"count","counters"},{"overflow","counters"},
            {"guard","guard"},{"giant","outside tile"},{"baby","outside tile"},{"reserved","record"},{"target","record"},{"false_match","CPU verification"}}) {
            backend::HipBsgsExecutor e(0,table,targets,verifier);
            auto ticket=e.submit(batch); e.drain();
            backend::bsgs_search_test_corruption=fault.first;
            expect_error([&] { (void)e.take(ticket); },fault.second);
            expect_error([&] { e.submit(batch); },"executor failed");
            ++tested;
        }
        backend::HipBsgsExecutor recovered(0,table,targets,verifier);
        auto ticket=recovered.submit(batch); recovered.drain();
        const auto result=recovered.take(ticket);
        if (result.verified_steps!=1 || result.matches.size()!=1) throw std::runtime_error("recovery failed");
        std::cout<<tested<<" HIP BSGS failure boundaries passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
