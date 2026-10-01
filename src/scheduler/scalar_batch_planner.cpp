#include "keyhunt/scheduler/scalar_batch_planner.h"
#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {
using core::UInt256;
using core::ScalarInterval;
ScalarBatchOrder parse_scalar_batch_order(const std::string& value) {
    if(value=="forward")return ScalarBatchOrder::Forward;
    if(value=="both-ends")return ScalarBatchOrder::BothEnds;
    if(value=="dance")return ScalarBatchOrder::Dance;
    throw std::invalid_argument("batch-order must be forward, both-ends or dance");
}
const char* scalar_batch_order_name(ScalarBatchOrder order) {
    switch(order) {
    case ScalarBatchOrder::Forward:return "forward";
    case ScalarBatchOrder::BothEnds:return "both-ends";
    case ScalarBatchOrder::Dance:return "dance";
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
    }
    if(order_==ScalarBatchOrder::Dance && !gaps.empty()) {
        // Fix the midpoint once over the missing envelope. Subtract first so
        // even a near-256-bit endpoint cannot overflow while finding its half.
        const auto low=gaps.front().begin();
        pivot_=low.add(gaps.back().end().subtract(low).divmod(UInt256(2)).first);
    }
    for(const auto& gap:gaps) {
        // Splitting before reservations makes the middle phase an endpoint
        // lookup, and prevents any owner or batch from crossing the pivot.
        if(pivot_ && gap.begin()<*pivot_ && *pivot_<gap.end()) {
            remaining_.emplace(gap.begin(),Remaining{{gap.begin(),*pivot_},std::nullopt});
            remaining_.emplace(*pivot_,Remaining{{*pivot_,gap.end()},std::nullopt});
        } else remaining_.emplace(gap.begin(),Remaining{gap,std::nullopt});
    }
}
std::optional<ScalarPlannedBatch> ScalarBatchPlanner::plan(const UInt256& work_span,uint64_t max_steps) {
    if(work_span.is_zero() || !max_steps)throw std::invalid_argument("scalar work and batch bounds must be positive");
    if(remaining_.empty())return std::nullopt;
    high_=phase_==1;
    auto chosen=high_?std::prev(remaining_.end()):remaining_.begin();
    if(order_==ScalarBatchOrder::Dance && phase_==2) {
        // The upper half may be exhausted, or the pivot may lie in a saved
        // hole. Choose its next live endpoint, then fall back to global low.
        chosen=remaining_.lower_bound(*pivot_);
        if(chosen==remaining_.end())chosen=remaining_.begin();
    }
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
    // Preserve the original owner when two of the fronts meet inside it.
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
    if(order_==ScalarBatchOrder::BothEnds)phase_=(phase_+1)%2;
    else if(order_==ScalarBatchOrder::Dance)phase_=(phase_+1)%3;
    pending_.reset();
}
} // namespace keyhunt::scheduler
