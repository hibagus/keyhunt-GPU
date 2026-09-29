#include "keyhunt/scheduler/work_unit.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace keyhunt::core;
using namespace keyhunt::scheduler;

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class Exception, class Function> void rejects(Function action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const Exception&) { rejected = true; }
    require(rejected, message);
}
ExecutionIdentity identity() {
    ExecutionIdentity result;
    result.job_digest.fill(1);
    result.target_digest.fill(2);
    result.algorithm_digest.fill(3);
    result.assignment_id.fill(4);
    result.assignment_generation = 5;
    result.executor_generation = 6;
    return result;
}
}

int main() {
    const auto execution = identity();
    // Enumerate all candidates, crossing every level's boundaries. No search or
    // journal is involved; advancing here simulates successfully accepted work.
    for (unsigned start = 1; start <= 4; ++start)
    for (unsigned length = 1; length <= 9; ++length)
    for (unsigned width = 1; width <= 6; ++width)
    for (unsigned work_limit = 1; work_limit <= 5; ++work_limit)
    for (unsigned batch_limit = 1; batch_limit <= 4; ++batch_limit) {
        const BlockGrid grid(ScalarInterval(UInt256(start), UInt256(start+length)), UInt256(width));
        std::vector<unsigned> visits(length);
        for (uint64_t id = 0; id < grid.count().to_uint64(); ++id) {
            const auto block_id = UInt256(id);
            auto cursor = grid.block(block_id).begin();
            while (auto work = WorkUnit::plan(grid, block_id, cursor, work_limit, execution)) {
                require(work->interval().begin() == cursor, "work starts at authoritative cursor");
                require(work->block_interval().contains(work->interval()), "work inside parent block");
                require(work->step_count() <= work_limit, "bounded local work count");
                auto batch_cursor = cursor;
                while (auto batch = KernelBatch::plan(*work, batch_cursor, batch_limit)) {
                    require(batch->interval().begin() == batch_cursor, "batch adjacency");
                    require(batch->step_count() <= batch_limit, "bounded kernel count");
                    require(batch->work().identity() == execution, "execution identity propagated");
                    require(batch->work().block_id() == block_id, "block ID propagated");
                    for (uint64_t i = 0; i < batch->step_count(); ++i) {
                        const auto scalar = batch->scalar_at(i);
                        require(batch->interval().contains(scalar), "local index maps inside batch");
                        ++visits.at(scalar.to_uint64() - start);
                    }
                    rejects<std::out_of_range>([&] { batch->scalar_at(batch->step_count()); }, "reject first invalid local index");
                    batch_cursor = batch->interval().end();
                }
                require(batch_cursor == work->interval().end(), "batches reach work end");
                cursor = work->interval().end();
            }
            require(cursor == grid.block(block_id).end(), "work reaches block end");
        }
        for (auto count : visits) require(count == 1, "hierarchy covers each scalar exactly once");
    }

    const auto begin = UInt256::power_of_two(200).add(UInt256(17));
    const BlockGrid grid(ScalarInterval(begin, begin.add(UInt256(100))), UInt256(100));
    auto source_identity = execution;
    const auto work = *WorkUnit::plan(grid, UInt256(), begin, 30, source_identity);
    source_identity.executor_generation++;
    require(work.identity() == execution && work.identity() != source_identity, "work owns immutable identity snapshot");
    const auto replay = *WorkUnit::plan(grid, UInt256(), begin, 30, execution);
    require(work.interval().begin() == replay.interval().begin()
            && work.interval().end() == replay.interval().end(), "planning same cursor does not advance progress");
    const auto resumed_cursor = begin.add(UInt256(13));
    const auto resumed = *WorkUnit::plan(grid, UInt256(), resumed_cursor, 30, execution);
    require(resumed.interval().begin() == resumed_cursor, "resume at explicit nonzero offset");
    const auto batch = *KernelBatch::plan(work, resumed_cursor, 50);
    require(batch.step_count() == 17 && batch.interval().end() == work.interval().end(), "resume kernel clipped to work tail");
    require(batch.scalar_at(16) == begin.add(UInt256(29)), "full high scalar preserved");
    require(!batch.interval().contains(work.interval().end()), "exclusive candidate bound");
    const auto next = *WorkUnit::plan(grid, UInt256(), work.interval().end(), 7, execution);
    require(next.step_count() == 7 && work.step_count() == 30, "new sizing does not resize existing work");
    require(!WorkUnit::plan(grid, UInt256(), grid.root().end(), 1, execution), "end cursor returns no work");
    require(!KernelBatch::plan(work, work.interval().end(), 1), "end cursor returns no batch");
    rejects<std::invalid_argument>([&] { WorkUnit::plan(grid, UInt256(), begin, 0, execution); }, "reject zero work bound");
    rejects<std::invalid_argument>([&] { WorkUnit::plan(grid, UInt256(), grid.root().end(), 0, execution); }, "invalid bound still rejected at end");
    rejects<std::invalid_argument>([&] { KernelBatch::plan(work, begin, 0); }, "reject zero batch bound");
    rejects<std::out_of_range>([&] { WorkUnit::plan(grid, UInt256(1), begin, 1, execution); }, "reject unassigned block ID");
    for (const auto cursor : {begin.subtract(UInt256(1)), grid.root().end().add(UInt256(1))})
        rejects<std::out_of_range>([&] { WorkUnit::plan(grid, UInt256(), cursor, 1, execution); }, "reject cursor outside block");
    for (const auto cursor : {begin.subtract(UInt256(1)), work.interval().end().add(UInt256(1))})
        rejects<std::out_of_range>([&] { KernelBatch::plan(work, cursor, 1); }, "reject cursor outside work");

    for (unsigned field = 0; field < 7; ++field) {
        auto changed = execution;
        if (field == 0) changed.job_digest.back()++;
        if (field == 1) changed.target_digest.back()++;
        if (field == 2) changed.algorithm_digest.back()++;
        if (field == 3) changed.assignment_id.back()++;
        if (field == 4) changed.assignment_generation++;
        if (field == 5) changed.executor_generation++;
        if (field == 6) changed.algorithm = static_cast<WorkAlgorithm>(255);
        require(changed != execution, "every identity component participates in comparison");
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto invalid = execution;
        if (field == 0) invalid.assignment_id.fill(0);
        if (field == 1) invalid.assignment_generation = 0;
        if (field == 2) invalid.executor_generation = 0;
        if (field == 3) invalid.algorithm = static_cast<WorkAlgorithm>(255);
        rejects<std::invalid_argument>([&] { WorkUnit::plan(grid, UInt256(), begin, 1, invalid); }, "reject invalid execution metadata or mapping");
    }
    const BlockGrid domain(ScalarInterval(UInt256(1), scalar_order()), UInt256::power_of_two(255));
    const auto huge = *WorkUnit::plan(domain, UInt256(), UInt256(1), UINT64_MAX, execution);
    require(huge.step_count() == UINT64_MAX, "maximum local count without narrowing");
    const auto huge_batch = *KernelBatch::plan(huge, UInt256(1), UINT64_MAX);
    require(huge_batch.scalar_at(UINT64_MAX-1) == UInt256(UINT64_MAX), "last valid maximum local index");
    rejects<std::out_of_range>([&] { huge_batch.scalar_at(UINT64_MAX); }, "maximum invalid local index rejected");
    const BlockGrid singletons(domain.root(), UInt256(1));
    const auto high_id = UInt256::power_of_two(200);
    const auto high_work = *WorkUnit::plan(singletons, high_id, high_id.add(UInt256(1)), 10, execution);
    require(high_work.block_id() == high_id && high_work.step_count() == 1, "high block ID preserved in work");
    const auto near_order = scalar_order().subtract(UInt256(3));
    const BlockGrid last(ScalarInterval(near_order, scalar_order()), UInt256(UINT64_MAX));
    const auto tail = *WorkUnit::plan(last, UInt256(), near_order, UINT64_MAX, execution);
    require(tail.step_count() == 3 && tail.interval().end() == scalar_order(), "clip maximum request at curve order");
    std::cout << checks << " work planning checks passed\n";
}
