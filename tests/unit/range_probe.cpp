// Test-only line protocol for the independent Python integer oracle.
#include "keyhunt/core/exact_range.h"
#include "keyhunt/scheduler/block_grid.h"
#include "keyhunt/scheduler/work_unit.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace keyhunt::core;

namespace {
std::string evaluate(const std::vector<std::string>& words) {
    if ((words.size() == 7 && words[0] == "work")
        || (words.size() == 10 && words[0] == "batch")) {
        using namespace keyhunt::scheduler;
        const BlockGrid grid(
            ScalarInterval(UInt256::from_hex(words[1]), UInt256::from_hex(words[2])),
            UInt256::from_hex(words[3]));
        ExecutionIdentity identity;
        identity.assignment_id.back() = 1;
        identity.assignment_generation = 1;
        identity.executor_generation = 1;
        const auto work = WorkUnit::plan(grid, UInt256::from_hex(words[4]),
            UInt256::from_hex(words[5]), UInt256::from_hex(words[6]).to_uint64(), identity);
        if (!work) return "none";
        if (words[0] == "work")
            return work->interval().begin().hex() + " " + work->interval().end().hex()
                + " " + UInt256(work->step_count()).hex();
        const auto batch = KernelBatch::plan(*work, UInt256::from_hex(words[7]),
                                             UInt256::from_hex(words[8]).to_uint64());
        if (!batch) return "none";
        const auto scalar = batch->scalar_at(UInt256::from_hex(words[9]).to_uint64());
        return batch->interval().begin().hex() + " " + batch->interval().end().hex()
            + " " + UInt256(batch->step_count()).hex() + " " + scalar.hex();
    }
    if (words.size() == 5 && (words[0] == "grid" || words[0] == "locate")) {
        const keyhunt::scheduler::BlockGrid grid(
            ScalarInterval(UInt256::from_hex(words[1]), UInt256::from_hex(words[2])),
            UInt256::from_hex(words[3]));
        const auto value = UInt256::from_hex(words[4]);
        if (words[0] == "locate") return grid.block_containing(value).hex();
        const auto block = grid.block(value);
        return grid.count().hex() + " " + block.begin().hex() + " " + block.end().hex();
    }
    if (words.size() != 3) throw std::invalid_argument("expected operation and two values");
    const auto a = UInt256::from_hex(words[1]);
    const auto b = UInt256::from_hex(words[2]);
    const auto& operation = words[0];
    if (operation == "add") return a.add(b).hex();
    if (operation == "sub") return a.subtract(b).hex();
    if (operation == "mul") return a.multiply(b).hex();
    if (operation == "div") {
        const auto result = a.divmod(b);
        return result.first.hex() + " " + result.second.hex();
    }
    if (operation == "range") {
        const ScalarInterval range(a, b);
        return range.size().hex();
    }
    throw std::invalid_argument("unknown operation");
}
}

int main() {
    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word; input >> word;) words.push_back(word);
        try {
            const auto result = evaluate(words); // Avoid a partial response on failure.
            std::cout << "ok " << result << '\n';
        } catch (const std::exception&) {
            std::cout << "error\n";
        }
    }
}
