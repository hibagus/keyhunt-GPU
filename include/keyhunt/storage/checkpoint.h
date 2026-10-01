#pragma once
#include "keyhunt/storage/journal.h"
#include <array>
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/backend/gpu_hash160.h"
#include "keyhunt/backend/gpu_ethereum.h"
#include "keyhunt/backend/gpu_bsgs.h"

namespace keyhunt::storage {
struct CheckpointOptions {
    // Supervised devices share the journal but retain an exclusive block guard.
    // Standalone callers keep the original whole-journal exclusion by default.
    bool concurrent_blocks=false;
    unsigned work_unit_seconds=0; // 0 preserves fixed batches; supervised default is 180.
    uint64_t xpoint_steps=1048576,giant_steps=16384;
    uint32_t target_batch=64,candidate_capacity=1024,checkpoint_seconds=10;
};
struct BsgsGroupMetrics {
    uint64_t batches=0,overflows=0;
    UInt256 device_steps,verified_device_steps;
    double kernel_ms=0;
};
struct CheckpointSummary {
    UInt256 resumed_scalars,computed_scalars,device_steps,verified_device_steps;
    uint64_t match_observations=0,batches=0,overflows=0,checkpoints=0,work_units=0;
    unsigned bsgs_group_size=0; // Last dispatch only; zero if no BSGS batch ran.
    // Indices 0/1 describe groups 1/8 across every attempt, including overflow.
    std::array<BsgsGroupMetrics,2> bsgs_groups{};
    double checkpoint_ms=0;
    // Include failed overflow attempts in cost, but never in useful work. BSGS
    // device steps count target giants; scalar coverage is credited separately.
    double kernel_ms=0,download_ms=0,seed_ms=0,verification_ms=0,executor_wall_ms=0;
    double replay_kernel_ms=0,revalidation_ms=0;
    UInt256 download_bytes;
    uint64_t peak_device_allocation_bytes=0,peak_pinned_allocation_bytes=0;
    bool complete=false; // A graceful stop may leave a valid in-progress block.
};
// Called only after COMMIT. Throwing (e.g. a broken stdout) stops submissions;
// the accepted state is still durable and discoverable on the next invocation.
using CheckpointObserver=std::function<void(const std::vector<ScalarInterval>&,size_t,double)>;
// Cleanup must drain outstanding GPU work before releasing the block lock,
// including exceptions. Standalone callers destroy their executor; a supervised
// device may retain drained allocations while its process owner lock stays held.
using CheckpointCleanup=std::function<void()>;
using XPointRunner=std::function<backend::XPointResult(const scheduler::KernelBatch&)>;
enum class CheckpointRequest { Run, Pause, Stop };
enum class CheckpointActivity { Idle, Draining, Paused, Running, Stopped, Completed };
// Only the owner thread calls these callbacks, outside SQL transactions and GPU
// submissions. poll is nonblocking; wait must wake periodically to observe stop.
// A paused owner retains its lock and BSGS subgroup cursor in memory.
struct CheckpointControl {
    std::function<CheckpointRequest()> poll;
    std::function<void(CheckpointActivity)> notify;
    std::function<void()> wait;
    std::function<void(const ScalarInterval&)> work_unit;
};
using BsgsRunner=std::function<backend::BsgsSearchResult(const core::BsgsBatch&)>;

// One bounded owner per local journal. The injected runner is the trusted
// executor interface, not an API for accepting remote claims of completed work.
// Production uses HIP; CPU mock runners exercise the same commit/replay logic.
class CheckpointRun {
public:
    static Scope create_xpoint(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::XPointTargets&);
    static Scope create_hash160(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::Hash160Targets&);
    static Scope create_ethereum(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::EthereumTargets&);
    static Scope create_bsgs(Journal&,const std::string& project,ScalarInterval root,UInt256 width,
        const core::BsgsPublicKeyTargets&,const bsgs::Table&);
    static CheckpointSummary xpoint(Journal&,const Grant&,const core::XPointTargets&,
        const core::XPointVerifier&,const XPointRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={},CheckpointControl={});
    static CheckpointSummary hash160(Journal&,const Grant&,const core::Hash160Targets&,
        const core::XPointVerifier&,const XPointRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={},CheckpointControl={});
    static CheckpointSummary ethereum(Journal&,const Grant&,const core::EthereumTargets&,
        const core::XPointVerifier&,const XPointRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={},CheckpointControl={});
    static CheckpointSummary bsgs(Journal&,const Grant&,const core::BsgsPublicKeyTargets&,const bsgs::Table&,
        const core::XPointVerifier&,const BsgsRunner&,CheckpointOptions={},CheckpointObserver={},CheckpointCleanup={},CheckpointControl={});
private:
    struct Impl;
};
} // namespace keyhunt::storage
