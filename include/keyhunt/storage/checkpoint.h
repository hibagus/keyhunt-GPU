#pragma once
#include "keyhunt/storage/journal.h"
#include "keyhunt/backend/hip_xpoint.h"
#include "keyhunt/backend/hip_bsgs.h"

namespace keyhunt::storage {
struct CheckpointOptions {
    uint64_t xpoint_steps=1048576,giant_steps=16384;
    uint32_t target_batch=64,candidate_capacity=1024,checkpoint_seconds=10;
};
struct CheckpointSummary {
    UInt256 resumed_scalars,computed_scalars,device_steps;
    uint64_t match_observations=0,batches=0,overflows=0,checkpoints=0;
    double checkpoint_ms=0;
};
// Called only after COMMIT. Throwing (e.g. a broken stdout) stops submissions;
// the accepted state is still durable and discoverable on the next invocation.
using CheckpointObserver=std::function<void(const std::vector<ScalarInterval>&,size_t,double)>;
// Cleanup must stop/drain and destroy the executor. It runs before the local
// owner lock is released, including exceptions from submission or output.
using CheckpointCleanup=std::function<void()>;
using XPointRunner=std::function<backend::XPointResult(const scheduler::KernelBatch&)>;
using BsgsRunner=std::function<backend::BsgsSearchResult(const core::BsgsBatch&)>;

// One bounded owner per local journal. The injected runner is the trusted
// executor interface, not an API for accepting remote claims of completed work.
// Production uses HIP; CPU mock runners exercise the same commit/replay logic.
class CheckpointRun {
public:
    static Scope create_xpoint(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::XPointTargets&);
    static Scope create_bsgs(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::BsgsTargets&,const bsgs::Table&);
    static CheckpointSummary xpoint(Journal&,const Grant&,const core::XPointTargets&,
        const core::XPointVerifier&,const XPointRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={});
    static CheckpointSummary bsgs(Journal&,const Grant&,const core::BsgsTargets&,const bsgs::Table&,
        const core::XPointVerifier&,const BsgsRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={});
private:
    struct Impl;
};
} // namespace keyhunt::storage
