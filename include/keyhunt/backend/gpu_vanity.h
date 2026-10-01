#pragma once
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/core/vanity_search.h"

namespace keyhunt::backend {
// Scalar search uses the same launch options and verified receipt layout.
using VanityOptions = XPointOptions;
using VanityResult = XPointResult;
// One slot, immutable targets, explicit stream ownership. The caller keeps the
// CPU verifier alive. No slot reuse before take; failures poison the executor.
// Single host owner; no device-wide synchronization or partition assumptions.
class GpuVanityExecutor {
public:
    GpuVanityExecutor(int device, core::VanityTargets targets,
        const core::XPointVerifier& verifier, VanityOptions options = {});
    ~GpuVanityExecutor();
    GpuVanityExecutor(const GpuVanityExecutor&) = delete;
    GpuVanityExecutor& operator=(const GpuVanityExecutor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    VanityResult take(Ticket ticket);
    void drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
