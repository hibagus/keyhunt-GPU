#include "keyhunt/scheduler/work_unit.h"

#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {

using core::UInt256;
using core::ScalarInterval;

namespace {
std::optional<ScalarInterval> bounded_interval(const ScalarInterval& parent,
                                              const UInt256& cursor, uint64_t max_steps) {
    if (!max_steps) throw std::invalid_argument("step bound must be positive");
    if (cursor < parent.begin() || cursor > parent.end())
        throw std::out_of_range("cursor outside parent interval");
    if (cursor == parent.end()) return std::nullopt;
    const auto span = std::min(UInt256(max_steps), parent.end().subtract(cursor));
    return ScalarInterval(cursor, cursor.add(span));
}

void validate(const ExecutionIdentity& identity) {
    if (identity.algorithm != WorkAlgorithm::DirectXPointV1 && identity.algorithm != WorkAlgorithm::DirectHash160V1)
        throw std::invalid_argument("unsupported work algorithm mapping");
    if (!identity.assignment_generation || !identity.executor_generation)
        throw std::invalid_argument("execution generations must be positive");
    if (std::all_of(identity.assignment_id.begin(), identity.assignment_id.end(),
                    [](uint8_t value) { return value == 0; }))
        throw std::invalid_argument("assignment ID must not be nil");
}
}

bool ExecutionIdentity::operator==(const ExecutionIdentity& other) const {
    return job_digest == other.job_digest && target_digest == other.target_digest
        && algorithm_digest == other.algorithm_digest && assignment_id == other.assignment_id
        && assignment_generation == other.assignment_generation
        && executor_generation == other.executor_generation && algorithm == other.algorithm;
}

WorkUnit::WorkUnit(ExecutionIdentity identity, UInt256 block_id,
                  ScalarInterval block_interval, ScalarInterval interval)
    : identity_(identity), block_id_(block_id), block_interval_(block_interval), interval_(interval) {}

std::optional<WorkUnit> WorkUnit::plan(const BlockGrid& grid, const UInt256& block_id,
    const UInt256& cursor, uint64_t max_steps, const ExecutionIdentity& identity) {
    validate(identity);
    const auto block = grid.block(block_id);
    const auto interval = bounded_interval(block, cursor, max_steps);
    if (!interval) return std::nullopt;
    return WorkUnit(identity, block_id, block, *interval);
}

KernelBatch::KernelBatch(WorkUnit work, ScalarInterval interval) : work_(work), interval_(interval) {}

std::optional<KernelBatch> KernelBatch::plan(const WorkUnit& work,
    const UInt256& cursor, uint64_t max_steps) {
    const auto interval = bounded_interval(work.interval(), cursor, max_steps);
    if (!interval) return std::nullopt;
    return KernelBatch(work, *interval);
}

UInt256 KernelBatch::scalar_at(uint64_t local_index) const {
    if (local_index >= step_count()) throw std::out_of_range("local index outside batch");
    return interval_.begin().add(UInt256(local_index));
}

} // namespace keyhunt::scheduler
