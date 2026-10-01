#pragma once

#include "keyhunt/scheduler/block_grid.h"
#include "keyhunt/core/scalar_stride.h"

#include <array>
#include <optional>

namespace keyhunt::scheduler {

using Digest = std::array<uint8_t, 32>;
using AssignmentId = std::array<uint8_t, 16>;

// Direct scalar mappings use k = begin + local_index. Minikeys use that same
// integer coordinate as an ordinal; hashing derives an unrelated private scalar.
// ReverseMinikeysV1 is a local lane mapping (end-1-index), with no persisted
// scalar-stride configuration or change to canonical ordinal receipt coordinates.
enum class WorkAlgorithm : uint8_t { DirectXPointV1 = 1, DirectHash160V1 = 2, DirectEthereumV1 = 3, DirectVanityV1 = 4, DirectMinikeysV1 = 5, StridedXPointV1 = 6, StridedHash160V1 = 7, StridedEthereumV1 = 8, StridedVanityV1 = 9, ReverseXPointV1 = 10, ReverseHash160V1 = 11, ReverseEthereumV1 = 12, ReverseVanityV1 = 13, OrbitXPointV1 = 14, OrbitHash160V1 = 15, OrbitEthereumV1 = 16, OrbitVanityV1 = 17, ReverseOrbitXPointV1 = 18, ReverseOrbitHash160V1 = 19, ReverseOrbitEthereumV1 = 20, ReverseOrbitVanityV1 = 21, ReverseMinikeysV1 = 22 };
bool is_strided(WorkAlgorithm);
bool is_reverse(WorkAlgorithm); // Indexed scalar mappings only; minikey order is execution policy.
bool is_minikeys(WorkAlgorithm);
bool is_orbit(WorkAlgorithm);
WorkAlgorithm scalar_family(WorkAlgorithm);
WorkAlgorithm strided_algorithm(WorkAlgorithm, bool reverse=false, bool orbit=false);

struct ExecutionIdentity {
    Digest job_digest{};
    Digest target_digest{};
    Digest algorithm_digest{}; // fingerprint of the resolved algorithm configuration
    AssignmentId assignment_id{};
    uint64_t assignment_generation = 0;
    uint64_t executor_generation = 0;
    WorkAlgorithm algorithm = WorkAlgorithm::DirectXPointV1;

    std::optional<core::ScalarStride> stride_mapping; // present exactly for indexed scalar algorithms

    bool operator==(const ExecutionIdentity& other) const;
    bool operator!=(const ExecutionIdentity& other) const { return !(*this == other); }
};

// A description of planned work, never evidence of completed/durable coverage.
// The owner supplies an authorized assignment and an authoritative cursor.
// Reverse minikey cursors are exclusive high endpoints; other cursors are low.
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
    // Receipt coordinates can be candidate indices; scalar_at always derives
    // the actual private scalar for a scalar-search family.
    core::UInt256 coordinate_at(uint64_t local_index) const;
    core::UInt256 scalar_at(uint64_t local_index) const;
    core::UInt256 scalar_stride() const;
    bool scalar_reverse() const;
    bool scalar_orbit() const;
    unsigned orbit_variant() const;
    core::UInt256 seed_scalar_at(uint64_t local_index) const;
    core::UInt256 ordinal_at(uint64_t local_index) const;
    bool ordinal_reverse() const;

private:
    KernelBatch(WorkUnit work, core::ScalarInterval interval);
    WorkUnit work_; // snapshot, including both generations and the parent bounds
    core::ScalarInterval interval_;
};

} // namespace keyhunt::scheduler
