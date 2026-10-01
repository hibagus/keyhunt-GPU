#include "keyhunt/scheduler/work_unit.h"

#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {

using core::UInt256;
using core::ScalarInterval;

bool is_strided(WorkAlgorithm algorithm) {
    return algorithm==WorkAlgorithm::StridedXPointV1 || algorithm==WorkAlgorithm::StridedHash160V1 ||
        algorithm==WorkAlgorithm::StridedEthereumV1 || algorithm==WorkAlgorithm::StridedVanityV1;
}
bool is_orbit(WorkAlgorithm algorithm) {
    return algorithm>=WorkAlgorithm::OrbitXPointV1 && algorithm<=WorkAlgorithm::ReverseOrbitVanityV1;
}
bool is_reverse(WorkAlgorithm algorithm) {
    return algorithm==WorkAlgorithm::ReverseXPointV1 || algorithm==WorkAlgorithm::ReverseHash160V1 ||
        algorithm==WorkAlgorithm::ReverseEthereumV1 || algorithm==WorkAlgorithm::ReverseVanityV1 ||
        (algorithm>=WorkAlgorithm::ReverseOrbitXPointV1 && algorithm<=WorkAlgorithm::ReverseOrbitVanityV1);
}
WorkAlgorithm scalar_family(WorkAlgorithm algorithm) {
    switch (algorithm) {
    case WorkAlgorithm::OrbitXPointV1:
    case WorkAlgorithm::ReverseOrbitXPointV1:
    case WorkAlgorithm::ReverseXPointV1:
    case WorkAlgorithm::StridedXPointV1:return WorkAlgorithm::DirectXPointV1;
    case WorkAlgorithm::OrbitHash160V1:
    case WorkAlgorithm::ReverseOrbitHash160V1:
    case WorkAlgorithm::ReverseHash160V1:
    case WorkAlgorithm::StridedHash160V1:return WorkAlgorithm::DirectHash160V1;
    case WorkAlgorithm::OrbitEthereumV1:
    case WorkAlgorithm::ReverseOrbitEthereumV1:
    case WorkAlgorithm::ReverseEthereumV1:
    case WorkAlgorithm::StridedEthereumV1:return WorkAlgorithm::DirectEthereumV1;
    case WorkAlgorithm::OrbitVanityV1:
    case WorkAlgorithm::ReverseOrbitVanityV1:
    case WorkAlgorithm::ReverseVanityV1:
    case WorkAlgorithm::StridedVanityV1:return WorkAlgorithm::DirectVanityV1;
    default:return algorithm;
    }
}
WorkAlgorithm strided_algorithm(WorkAlgorithm algorithm, bool reverse, bool orbit) {
    switch (algorithm) {
    case WorkAlgorithm::DirectXPointV1:return orbit?(reverse?WorkAlgorithm::ReverseOrbitXPointV1:WorkAlgorithm::OrbitXPointV1):reverse?WorkAlgorithm::ReverseXPointV1:WorkAlgorithm::StridedXPointV1;
    case WorkAlgorithm::DirectHash160V1:return orbit?(reverse?WorkAlgorithm::ReverseOrbitHash160V1:WorkAlgorithm::OrbitHash160V1):reverse?WorkAlgorithm::ReverseHash160V1:WorkAlgorithm::StridedHash160V1;
    case WorkAlgorithm::DirectEthereumV1:return orbit?(reverse?WorkAlgorithm::ReverseOrbitEthereumV1:WorkAlgorithm::OrbitEthereumV1):reverse?WorkAlgorithm::ReverseEthereumV1:WorkAlgorithm::StridedEthereumV1;
    case WorkAlgorithm::DirectVanityV1:return orbit?(reverse?WorkAlgorithm::ReverseOrbitVanityV1:WorkAlgorithm::OrbitVanityV1):reverse?WorkAlgorithm::ReverseVanityV1:WorkAlgorithm::StridedVanityV1;
    default:throw std::invalid_argument("family does not support scalar strides");
    }
}

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
    if ((is_strided(identity.algorithm)||is_reverse(identity.algorithm)||is_orbit(identity.algorithm))!=identity.stride_mapping.has_value() ||
        (identity.stride_mapping && (identity.stride_mapping->reverse()!=is_reverse(identity.algorithm) ||
                                    identity.stride_mapping->orbit()!=is_orbit(identity.algorithm))))
        throw std::invalid_argument("stride mapping and algorithm disagree");
    const auto family=scalar_family(identity.algorithm);
    if (family != WorkAlgorithm::DirectXPointV1 && family != WorkAlgorithm::DirectHash160V1 &&
        family != WorkAlgorithm::DirectEthereumV1 && family != WorkAlgorithm::DirectVanityV1 && family != WorkAlgorithm::DirectMinikeysV1)
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
        && executor_generation == other.executor_generation && algorithm == other.algorithm
        && stride_mapping == other.stride_mapping;
}

WorkUnit::WorkUnit(ExecutionIdentity identity, UInt256 block_id,
                  ScalarInterval block_interval, ScalarInterval interval)
    : identity_(identity), block_id_(block_id), block_interval_(block_interval), interval_(interval) {}

std::optional<WorkUnit> WorkUnit::plan(const BlockGrid& grid, const UInt256& block_id,
    const UInt256& cursor, uint64_t max_steps, const ExecutionIdentity& identity) {
    validate(identity);
    if (identity.stride_mapping && !identity.stride_mapping->indices().contains(grid.root()))
        throw std::invalid_argument("grid exceeds strided candidate indices");
    const auto block = grid.block(block_id);
    const auto interval = bounded_interval(block, cursor, max_steps);
    if (!interval) return std::nullopt;
    return WorkUnit(identity, block_id, block, *interval);
}

KernelBatch::KernelBatch(WorkUnit work, ScalarInterval interval) : work_(work), interval_(interval) {}

std::optional<KernelBatch> KernelBatch::plan(const WorkUnit& work,
    const UInt256& cursor, uint64_t max_steps) {
    // Keep every device batch within one orbit variant. Arbitrary block/work
    // boundaries can cut a variant; callers still receive its exact prefix.
    auto interval = bounded_interval(work.interval(), cursor, max_steps);
    const auto& mapping=work.identity().stride_mapping;
    if(interval && mapping && mapping->orbit())
        interval=ScalarInterval(cursor,std::min(interval->end(),mapping->variant_end(cursor)));
    if (!interval) return std::nullopt;
    return KernelBatch(work, *interval);
}

UInt256 KernelBatch::coordinate_at(uint64_t local_index) const {
    if (local_index >= step_count()) throw std::out_of_range("local index outside batch");
    return interval_.begin().add(UInt256(local_index));
}
UInt256 KernelBatch::scalar_at(uint64_t local_index) const {
    if(work_.identity().algorithm==WorkAlgorithm::DirectMinikeysV1)throw std::logic_error("minikey work uses ordinal_at");
    const auto coordinate=coordinate_at(local_index);
    const auto& mapping=work_.identity().stride_mapping;
    return mapping ? mapping->scalar(coordinate) : coordinate;
}
UInt256 KernelBatch::scalar_stride() const {
    const auto& mapping=work_.identity().stride_mapping;
    return mapping ? mapping->stride() : UInt256(1);
}

bool KernelBatch::scalar_reverse() const {
    const auto& mapping=work_.identity().stride_mapping;
    return mapping && mapping->reverse();
}

bool KernelBatch::scalar_orbit() const {
    const auto& mapping=work_.identity().stride_mapping;
    return mapping && mapping->orbit();
}
unsigned KernelBatch::orbit_variant() const {
    const auto& mapping=work_.identity().stride_mapping;
    return mapping?mapping->variant(interval_.begin()):0;
}
UInt256 KernelBatch::seed_scalar_at(uint64_t local_index) const {
    const auto& mapping=work_.identity().stride_mapping;
    return mapping?mapping->seed(coordinate_at(local_index)):scalar_at(local_index);
}

UInt256 KernelBatch::ordinal_at(uint64_t local_index) const {
    if(work_.identity().algorithm!=WorkAlgorithm::DirectMinikeysV1)throw std::logic_error("scalar work uses scalar_at");
    if(local_index>=step_count())throw std::out_of_range("local index outside batch");
    return interval_.begin().add(UInt256(local_index));
}
} // namespace keyhunt::scheduler
