#pragma once
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/core/ethereum_search.h"

namespace keyhunt::backend {
// Scalar search uses the same launch options and verified receipt layout.
using EthereumOptions = XPointOptions;
using EthereumResult = XPointResult;
// One slot, immutable targets, explicit stream ownership. The caller keeps the
// CPU verifier alive. No slot reuse before take; failures poison the executor.
// Single host owner; no device-wide synchronization or partition assumptions.
class GpuEthereumExecutor {
public:
    GpuEthereumExecutor(int device, core::EthereumTargets targets,
        const core::XPointVerifier& verifier, EthereumOptions options = {});
    ~GpuEthereumExecutor();
    GpuEthereumExecutor(const GpuEthereumExecutor&) = delete;
    GpuEthereumExecutor& operator=(const GpuEthereumExecutor&) = delete;
    Ticket submit(const scheduler::KernelBatch& batch);
    bool poll(Ticket ticket);
    EthereumResult take(Ticket ticket);
    void drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
