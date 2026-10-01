#pragma once
#include "keyhunt/backend/gpu_executor.h"
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::backend {
enum class XPointKernel { Direct, Stepped };
struct XPointOptions {
    uint64_t max_steps = 1048576;
    core::UInt256 stride{1}; // immutable point-cache step; batch identity must agree
    bool reverse=false; // reverse walks use -SG, while the stride stays positive
    XPointKernel kernel = XPointKernel::Stepped;
    uint32_t candidate_capacity = 1024;
    uint64_t memory_reserve_bytes = 64*1024*1024;
};
struct XPointResult {
    scheduler::KernelBatch batch;
    std::vector<core::XPointMatch> matches;
    bool overflow = false;
    // An overflow attempt has zero verified_steps and no consumable matches.
    // These are volatile execution receipts, never durable search coverage.
    uint64_t verified_steps = 0, device_steps = 0, candidate_count = 0;
    uint64_t launch_count = 1, device_allocation_bytes = 0, pinned_allocation_bytes = 0, download_bytes = 0;
    float kernel_ms = 0, download_ms = 0;
    double seed_ms = 0, verification_ms = 0, wall_ms = 0;
};
// One slot, immutable targets, explicit stream ownership. The caller keeps the
// CPU verifier alive. No slot reuse before take; failures poison the executor.
// Single host owner; no device-wide synchronization or partition assumptions.
class GpuXPointExecutor {
public:
    GpuXPointExecutor(int device, core::XPointTargets targets,
        const core::XPointVerifier& verifier, XPointOptions options = {});
    ~GpuXPointExecutor();
    GpuXPointExecutor(const GpuXPointExecutor&) = delete;
    GpuXPointExecutor& operator=(const GpuXPointExecutor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    XPointResult take(Ticket ticket);
    void drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
