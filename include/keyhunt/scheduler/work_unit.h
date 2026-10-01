#pragma once

#include "keyhunt/scheduler/block_grid.h"

#include <array>
#include <optional>

namespace keyhunt::scheduler {

using Digest = std::array<uint8_t, 32>;
using AssignmentId = std::array<uint8_t, 16>;

// The first executor mapping is k = batch.begin + local_index. BSGS, strides,
// endomorphisms and minikey ordinals need separately validated mappings.
enum class WorkAlgorithm : uint8_t { DirectXPointV1 = 1, DirectHash160V1 = 2, DirectEthereumV1 = 3 };

struct ExecutionIdentity {
    Digest job_digest{};
    Digest target_digest{};
    Digest algorithm_digest{}; // fingerprint of the resolved algorithm configuration
    AssignmentId assignment_id{};
    uint64_t assignment_generation = 0;
    uint64_t executor_generation = 0;
    WorkAlgorithm algorithm = WorkAlgorithm::DirectXPointV1;

    bool operator==(const ExecutionIdentity& other) const;
    bool operator!=(const ExecutionIdentity& other) const { return !(*this == other); }
};

// A description of planned work, never evidence of completed/durable coverage.
// The owner supplies an authorized assignment and an authoritative cursor.
class WorkUnit {
public:
    static std::optional<WorkUnit> plan(const BlockGrid& grid, const core::UInt256& block_id,
        const core::UInt256& cursor, uint64_t max_steps, const ExecutionIdentity& identity);

    const ExecutionIdentity& identity() const { return identity_; }
    const core::UInt256& block_id() const { return block_id_; }
    const core::ScalarInterval& block_interval() const { return block_interval_; }
    const core::ScalarInterval& interval() const { return interval_; }
    uint64_t step_count() const { return interval_.size().to_uint64(); }

private:
    WorkUnit(ExecutionIdentity identity, core::UInt256 block_id,
             core::ScalarInterval block_interval, core::ScalarInterval interval);
    ExecutionIdentity identity_;
    core::UInt256 block_id_;
    core::ScalarInterval block_interval_, interval_;
};

class KernelBatch {
public:
    static std::optional<KernelBatch> plan(const WorkUnit& work,
        const core::UInt256& cursor, uint64_t max_steps);

    const WorkUnit& work() const { return work_; }
    const core::ScalarInterval& interval() const { return interval_; }
    uint64_t step_count() const { return interval_.size().to_uint64(); }
    core::UInt256 scalar_at(uint64_t local_index) const;

private:
    KernelBatch(WorkUnit work, core::ScalarInterval interval);
    WorkUnit work_; // snapshot, including both generations and the parent bounds
    core::ScalarInterval interval_;
};

} // namespace keyhunt::scheduler
