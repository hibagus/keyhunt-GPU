#include "keyhunt/backend/gpu_executor.h"
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
        int count = 0;
        backend::gpu_check(gpuGetDeviceCount(&count), "device count");
        backend::gpu_check(gpuSetDevice(0), "select original device");
        for (const char* operation : {"gpuGetDevice", "gpuSetDevice"}) {
            // With two visible GPUs this proves restoration after a successful
            // native selection followed by a throwing constructor check.
            backend::injected_failure = operation;
            expect_error([&] { backend::DeviceScope scope(count > 1 ? 1 : 0); }, operation);
            int current = -1;
            backend::gpu_check(gpuGetDevice(&current), "check restored device");
            if (current != 0) throw std::runtime_error("failed scope changed caller device");
            ++tested;
        }
        for (const char* operation : {"gpuStreamCreateWithFlags", "gpuEventCreate(start)",
                "gpuEventCreate(kernel_done)", "gpuEventCreate(done)", "gpuMalloc(output)",
                "gpuMalloc(count)", "gpuHostMalloc(output)", "gpuHostMalloc(count)"}) {
            backend::injected_failure = operation;
            expect_error([] { backend::GpuDiagnosticExecutor e(0); }, operation);
            ++tested;
        }
        for (const char* operation : {"gpuMemsetAsync(output)", "gpuMemsetAsync(count)",
                "gpuEventRecord(start)", "diagnostic_indices launch", "gpuEventRecord(kernel_done)",
                "gpuMemcpyAsync(output)", "gpuMemcpyAsync(count)", "gpuEventRecord(done)",
                "gpuEventQuery", "gpuEventElapsedTime(kernel)", "gpuEventElapsedTime(download)",
                "gpuStreamSynchronize"}) {
            backend::GpuDiagnosticExecutor e(0);
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
        backend::GpuDiagnosticExecutor recovered(0);
        auto ticket = recovered.submit(batch); recovered.drain();
        auto result = recovered.take(ticket);
        if (result.device_steps != 257 || result.scalars.front() != UInt256(1))
            throw std::runtime_error("fresh executor did not recover after injected errors");
        std::cout << tested << " GPU failure boundaries passed; fresh executor verified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
