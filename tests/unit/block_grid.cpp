#include "keyhunt/scheduler/block_grid.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace keyhunt::core;
using keyhunt::scheduler::BlockGrid;

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
}

int main() {
    // Enumerate the actual union in a small domain; every scalar must be visited
    // exactly once, independently of the block formula under test.
    for (unsigned begin = 1; begin <= 16; ++begin) {
        for (unsigned end = begin + 1; end <= 24; ++end) {
            for (unsigned width = 1; width <= 32; ++width) {
                const BlockGrid grid(ScalarInterval{UInt256(begin), UInt256(end)}, UInt256(width));
                std::vector<unsigned> visits(end - begin, 0);
                auto previous = grid.root().begin();
                for (uint64_t id = 0; id < grid.count().to_uint64(); ++id) {
                    const auto block = grid.block(UInt256(id));
                    require(block.begin() == previous, "adjacent blocks have no gap or overlap");
                    require(grid.root().contains(block), "block within root");
                    require(block.size() <= grid.width(), "block at most width");
                    for (auto scalar = block.begin().to_uint64(); scalar < block.end().to_uint64(); ++scalar) {
                        ++visits.at(scalar - begin);
                        require(grid.block_containing(UInt256(scalar)) == UInt256(id), "inverse block lookup");
                    }
                    previous = block.end();
                }
                require(previous == grid.root().end(), "last block reaches exclusive end");
                for (auto seen : visits) require(seen == 1, "every scalar visited exactly once");
                rejects<std::out_of_range>([&] { grid.block(grid.count()); }, "reject past final block");
            }
        }
    }
    const ScalarInterval domain(UInt256(1), scalar_order());
    const auto maximum = UInt256::from_hex(std::string(64, 'f'));
    const BlockGrid large_width(domain, maximum);
    require(large_width.count() == UInt256(1), "larger-than-job width yields one block");
    require(large_width.block(UInt256()).end() == scalar_order(), "clip before overflowing addition");
    const BlockGrid singletons(domain, UInt256(1));
    const auto high_id = UInt256::power_of_two(200).add(UInt256(19));
    const auto high_block = singletons.block(high_id);
    require(singletons.count() == domain.size(), "256-bit block count");
    require(high_block.begin() == high_id.add(UInt256(1)), "block ID above 64 bits retained");
    require(singletons.block_containing(high_block.begin()) == high_id, "wide ID roundtrip");
    const auto last_id = singletons.count().subtract(UInt256(1));
    require(singletons.block(last_id).end() == scalar_order(), "last valid high block");
    rejects<std::invalid_argument>([&] { BlockGrid invalid(domain, UInt256()); }, "reject zero width");
    rejects<std::out_of_range>([&] { singletons.block(maximum); }, "reject invalid wide ID before multiplying");
    rejects<std::out_of_range>([&] { singletons.block_containing(UInt256()); }, "reject scalar below root");
    rejects<std::out_of_range>([&] { singletons.block_containing(scalar_order()); }, "exclusive root end has no block");
    const BlockGrid example(ScalarInterval(UInt256(1000), UInt256(1100)), UInt256(32));
    require(example.count() == UInt256(4), "documented example count");
    const auto tail = example.block(UInt256(3));
    require(tail.begin() == UInt256(1096) && tail.end() == UInt256(1100), "documented shorter tail");
    std::cout << checks << " block grid checks passed\n";
}
