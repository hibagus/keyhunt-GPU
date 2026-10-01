#pragma once
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/core/minikey_search.h"

namespace keyhunt::backend {
// Consecutive candidate strings hash to unrelated scalars; no point-walk option.
struct MinikeysOptions {
    uint64_t max_steps=1048576;
    uint32_t candidate_capacity=1024;
    uint64_t memory_reserve_bytes=64*1024*1024;
};
// Match coordinates retain ordinals for CPU reconstruction and journal coverage.
using MinikeysResult = XPointResult;
// One slot, immutable targets, explicit stream ownership. The caller keeps the
// CPU verifier alive. No slot reuse before take; failures poison the executor.
// Single host owner; no device-wide synchronization or partition assumptions.
class GpuMinikeysExecutor {
public:
    GpuMinikeysExecutor(int device, core::MinikeyTargets targets,
        const core::XPointVerifier& verifier, MinikeysOptions options = {});
    ~GpuMinikeysExecutor();
    GpuMinikeysExecutor(const GpuMinikeysExecutor&) = delete;
    GpuMinikeysExecutor& operator=(const GpuMinikeysExecutor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    MinikeysResult take(Ticket ticket);
    void drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
