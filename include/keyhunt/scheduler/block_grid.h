#pragma once

#include "keyhunt/core/exact_range.h"

namespace keyhunt::scheduler {

// Immutable geometry only. Does not enumerate blocks or record ownership/state.
class BlockGrid {
public:
    BlockGrid(core::ScalarInterval root, core::UInt256 width);
    const core::ScalarInterval& root() const { return root_; }
    const core::UInt256& width() const { return width_; }
    const core::UInt256& count() const { return count_; }
    core::ScalarInterval block(const core::UInt256& id) const;
    core::UInt256 block_containing(const core::UInt256& scalar) const;

private:
    core::ScalarInterval root_;
    core::UInt256 width_, count_;
};

} // namespace keyhunt::scheduler
