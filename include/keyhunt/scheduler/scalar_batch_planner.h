#pragma once
#include "keyhunt/scheduler/work_unit.h"
#include <map>
#include <string>
#include <vector>

namespace keyhunt::scheduler {
// Execution policy over existing receipt coordinates; never a scalar mapping.
enum class ScalarBatchOrder { Forward, BothEnds };
ScalarBatchOrder parse_scalar_batch_order(const std::string&);
const char* scalar_batch_order_name(ScalarBatchOrder);
struct ScalarPlannedBatch { KernelBatch batch; bool starts_work, finishes_work; };
class ScalarBatchPlanner {
public:
    ScalarBatchPlanner(BlockGrid, core::UInt256 block,
        const std::vector<core::ScalarInterval>& gaps, ExecutionIdentity, ScalarBatchOrder);
    std::optional<ScalarPlannedBatch> plan(const core::UInt256& work_span, uint64_t max_steps);
    void accept();
private:
    struct Remaining { core::ScalarInterval interval; std::optional<core::ScalarInterval> work; };
    BlockGrid grid_;
    core::UInt256 block_;
    ExecutionIdentity identity_;
    ScalarBatchOrder order_;
    bool high_=false;
    std::map<core::UInt256,Remaining> remaining_;
    std::optional<ScalarPlannedBatch> pending_;
    core::UInt256 selected_;
};
} // namespace keyhunt::scheduler
