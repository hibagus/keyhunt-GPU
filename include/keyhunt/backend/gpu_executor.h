#pragma once

#include "keyhunt/scheduler/work_unit.h"
#include <memory>
#include <vector>

namespace keyhunt::backend {

struct ExecutorOptions {
    uint64_t max_steps = 4096;
    uint64_t memory_reserve_bytes = 64 * 1024 * 1024;
};
struct Ticket {
    uint64_t executor = 0, sequence = 0;
};
struct DiagnosticResult {
    // This is the submitted plan, NOT completed search coverage. C07 only
    // exercises index mapping, transfers and lifecycle, with no curve search.
    scheduler::KernelBatch batch;
    std::vector<core::UInt256> scalars;
    uint64_t device_steps = 0, launch_count = 0;
    uint64_t device_allocation_bytes = 0, pinned_allocation_bytes = 0;
    uint64_t download_bytes = 0;
    float kernel_ms = 0, download_ms = 0;
    double wall_ms = 0, verification_ms = 0;
};

// One reusable slot bounds device memory, pinned memory and unconsumed results.
// Single host thread per executor. A result owns its copied data after take().
// Failed runtime operations poison the executor; create a new one to retry.
class GpuDiagnosticExecutor {
public:
    explicit GpuDiagnosticExecutor(int device, ExecutorOptions options = {});
    ~GpuDiagnosticExecutor();
    GpuDiagnosticExecutor(const GpuDiagnosticExecutor&) = delete;
    GpuDiagnosticExecutor& operator=(const GpuDiagnosticExecutor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    DiagnosticResult take(Ticket ticket); // requires ready; host-verifies every index
    void drain(); // waits for this stream only; retains the result for take()
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
