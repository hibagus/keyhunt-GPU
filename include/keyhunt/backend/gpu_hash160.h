#pragma once
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/core/hash160_search.h"

namespace keyhunt::backend {
// Scalar search uses the same launch options and verified receipt layout.
using Hash160Options = XPointOptions;
using Hash160Result = XPointResult;
// One slot, immutable targets, explicit stream ownership. The caller keeps the
// CPU verifier alive. No slot reuse before take; failures poison the executor.
// Single host owner; no device-wide synchronization or partition assumptions.
class GpuHash160Executor {
public:
    GpuHash160Executor(int device, core::Hash160Targets targets,
        const core::XPointVerifier& verifier, Hash160Options options = {});
    ~GpuHash160Executor();
    GpuHash160Executor(const GpuHash160Executor&) = delete;
    GpuHash160Executor& operator=(const GpuHash160Executor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    Hash160Result take(Ticket ticket);
    void drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
