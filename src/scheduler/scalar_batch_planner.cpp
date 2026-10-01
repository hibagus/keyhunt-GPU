#include "keyhunt/scheduler/scalar_batch_planner.h"
#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {
using core::UInt256;
using core::ScalarInterval;
ScalarBatchOrder parse_scalar_batch_order(const std::string& value) {
    if(value=="forward")return ScalarBatchOrder::Forward;
    if(value=="both-ends")return ScalarBatchOrder::BothEnds;
    throw std::invalid_argument("batch-order must be forward or both-ends");
}
const char* scalar_batch_order_name(ScalarBatchOrder order) {
    switch(order) {
    case ScalarBatchOrder::Forward:return "forward";
    case ScalarBatchOrder::BothEnds:return "both-ends";
    }
    throw std::invalid_argument("invalid scalar batch order");
}
ScalarBatchPlanner::ScalarBatchPlanner(BlockGrid grid,UInt256 block,
    const std::vector<ScalarInterval>& gaps,ExecutionIdentity identity,ScalarBatchOrder order)
    :grid_(std::move(grid)),block_(block),identity_(std::move(identity)),order_(order) {
    (void)scalar_batch_order_name(order);
    const auto family=scalar_family(identity_.algorithm);
    if(family!=WorkAlgorithm::DirectXPointV1 && family!=WorkAlgorithm::DirectHash160V1 &&
       family!=WorkAlgorithm::DirectEthereumV1 && family!=WorkAlgorithm::DirectVanityV1)
        throw std::invalid_argument("batch-order applies only to scalar search families");
    const auto parent=grid_.block(block_);
    // A completed checkpoint has no live executor generation. Validate work
    // identities only when there is missing coverage to submit.
    if(!gaps.empty())(void)WorkUnit::plan(grid_,block_,parent.begin(),1,identity_);
    for(size_t i=0;i<gaps.size();++i) {
        if(!parent.contains(gaps[i]) || (i && gaps[i-1].end()>gaps[i].begin()))
            throw std::invalid_argument("scalar gaps must be sorted, disjoint and inside the block");
        remaining_.emplace(gaps[i].begin(),Remaining{gaps[i],std::nullopt});
    }
}
std::optional<ScalarPlannedBatch> ScalarBatchPlanner::plan(const UInt256& work_span,uint64_t max_steps) {
    if(work_span.is_zero() || !max_steps)throw std::invalid_argument("scalar work and batch bounds must be positive");
    if(remaining_.empty())return std::nullopt;
    auto chosen=high_?std::prev(remaining_.end()):remaining_.begin();
    const bool starts=!chosen->second.work;
    if(starts) {
        const auto gap=chosen->second.interval;
        const auto span=std::min({work_span,gap.size(),UInt256(UINT64_MAX)});
        const ScalarInterval work=high_?ScalarInterval(gap.end().subtract(span),gap.end()):
            ScalarInterval(gap.begin(),gap.begin().add(span));
        remaining_.erase(chosen);
        if(work.size()!=gap.size()) {
            const ScalarInterval free=high_?ScalarInterval(gap.begin(),work.begin()):ScalarInterval(work.end(),gap.end());
            remaining_.emplace(free.begin(),Remaining{free,std::nullopt});
        }
        chosen=remaining_.emplace(work.begin(),Remaining{work,work}).first;
    }
    // Preserve the original owner while either front consumes its middle.
    const auto active=chosen->second.interval,reserved=*chosen->second.work;
    const auto work=*WorkUnit::plan(grid_,block_,reserved.begin(),reserved.size().to_uint64(),identity_);
    const auto steps=std::min(UInt256(max_steps),active.size());
    auto begin=high_?active.end().subtract(steps):active.begin();
    if(high_ && identity_.stride_mapping && identity_.stride_mapping->orbit()) {
        const auto& mapping=*identity_.stride_mapping;
        const auto variant_begin=mapping.variant_end(active.end().subtract(UInt256(1))).subtract(mapping.seed_count());
        // KernelBatch clips the upper edge of a forward batch. Clip its lower
        // edge here as well so a high selection stays attached to the high end.
        begin=std::max(begin,variant_begin);
    }
    const auto batch=*KernelBatch::plan(work,begin,high_?active.end().subtract(begin).to_uint64():steps.to_uint64());
    selected_=chosen->first;
    pending_=ScalarPlannedBatch{batch,starts,batch.interval().size()==active.size()};
    return pending_;
}
void ScalarBatchPlanner::accept() {
    if(!pending_)throw std::logic_error("no scalar batch to accept");
    const auto chosen=remaining_.find(selected_);const auto entry=chosen->second;
    const auto& batch=pending_->batch;
    remaining_.erase(chosen);
    if(!pending_->finishes_work) {
        const ScalarInterval rest=high_?ScalarInterval(entry.interval.begin(),batch.interval().begin()):
            ScalarInterval(batch.interval().end(),entry.interval.end());
        remaining_.emplace(rest.begin(),Remaining{rest,entry.work});
    }
    // Only accepted coverage advances the phase; overflow retries this endpoint.
    if(order_==ScalarBatchOrder::BothEnds)high_=!high_;
    pending_.reset();
}
} // namespace keyhunt::scheduler
