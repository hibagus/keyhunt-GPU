#pragma once
#include "keyhunt/scheduler/work_unit.h"
#include <map>
#include <string>
#include <vector>

namespace keyhunt::scheduler {
// Execution policy over existing receipt coordinates; never a scalar mapping.
enum class ScalarBatchOrder { Forward, BothEnds, Dance, RandomWindow };
struct ScalarRandomWindow { core::UInt256 seed{0}; unsigned tiles=64; };
ScalarRandomWindow parse_scalar_random_window(const std::string& seed,const std::string& window);
void validate_scalar_random_window(ScalarBatchOrder,const std::optional<ScalarRandomWindow>&);
ScalarBatchOrder parse_scalar_batch_order(const std::string&);
const char* scalar_batch_order_name(ScalarBatchOrder);
struct ScalarPlannedBatch { KernelBatch batch; bool starts_work, finishes_work; };
class ScalarBatchPlanner {
public:
    ScalarBatchPlanner(BlockGrid, core::UInt256 block,
        const std::vector<core::ScalarInterval>& gaps, ExecutionIdentity, ScalarBatchOrder,
        std::optional<ScalarRandomWindow> random=std::nullopt);
    std::optional<ScalarPlannedBatch> plan(const core::UInt256& work_span, uint64_t max_steps);
    void accept();
private:
    struct Remaining { core::ScalarInterval interval; std::optional<core::ScalarInterval> work; };
    BlockGrid grid_;
    core::UInt256 block_;
    ExecutionIdentity identity_;
    ScalarBatchOrder order_;
    unsigned phase_=0;
    bool high_=false;
    std::optional<core::UInt256> pivot_;
    std::map<core::UInt256,Remaining> remaining_;
    std::optional<ScalarPlannedBatch> pending_;
    core::UInt256 selected_;
    // Shuffling freezes tile boundaries. Overflow and orbit clipping consume
    // sub-batches of one tile before the next tile is selected.
    struct WindowTile { core::ScalarInterval remaining,work; bool starts_work,finishes_work; };
    ScalarRandomWindow random_;
    core::UInt256 random_counter_;
    std::vector<WindowTile> window_;
    size_t window_next_=0;
    unsigned random_below(unsigned bound);
    void fill_window(const core::UInt256& work_span,uint64_t max_steps);
};
} // namespace keyhunt::scheduler
