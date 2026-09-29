#include "keyhunt/backend/hip_executor.h"
#include "runtime.h"
#include <iostream>

using namespace keyhunt;
using core::UInt256;

template<class F> void expect_error(F fn, const std::string& text) {
    try { fn(); } catch (const std::exception& error) {
        if (std::string(error.what()).find(text) != std::string::npos) return;
        throw;
    }
    throw std::runtime_error("missing expected failure: " + text);
}
int main() {
    try {
        scheduler::BlockGrid grid(core::ScalarInterval(UInt256(1), UInt256(258)), UInt256(257));
        scheduler::ExecutionIdentity id;
        id.assignment_id[0] = 1; id.assignment_generation = id.executor_generation = 1;
        auto work = scheduler::WorkUnit::plan(grid, UInt256(0), UInt256(1), 257, id);
        auto batch = *scheduler::KernelBatch::plan(*work, UInt256(1), 257);
        unsigned tested = 0;
        for (const char* operation : {"hipStreamCreateWithFlags", "hipEventCreate(start)",
                "hipEventCreate(kernel_done)", "hipEventCreate(done)", "hipMalloc(output)",
                "hipMalloc(count)", "hipHostMalloc(output)", "hipHostMalloc(count)"}) {
            backend::injected_failure = operation;
            expect_error([] { backend::HipDiagnosticExecutor e(0); }, operation);
            ++tested;
        }
        for (const char* operation : {"hipMemsetAsync(output)", "hipMemsetAsync(count)",
                "hipEventRecord(start)", "diagnostic_indices launch", "hipEventRecord(kernel_done)",
                "hipMemcpyAsync(output)", "hipMemcpyAsync(count)", "hipEventRecord(done)",
                "hipEventQuery", "hipEventElapsedTime(kernel)", "hipEventElapsedTime(download)",
                "hipStreamSynchronize"}) {
            backend::HipDiagnosticExecutor e(0);
            backend::injected_failure = operation;
            expect_error([&] {
                auto ticket = e.submit(batch);
                e.drain();
                (void)e.take(ticket);
            }, operation);
            expect_error([&] { e.submit(batch); }, "executor failed");
            expect_error([&] { e.drain(); }, "executor failed");
            ++tested;
        }
        // Failure cleanup must leave other/new executors usable on the device.
        backend::HipDiagnosticExecutor recovered(0);
        auto ticket = recovered.submit(batch); recovered.drain();
        auto result = recovered.take(ticket);
        if (result.device_steps != 257 || result.scalars.front() != UInt256(1))
            throw std::runtime_error("fresh executor did not recover after injected errors");
        std::cout << tested << " HIP failure boundaries passed; fresh executor verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
