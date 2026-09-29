#include "keyhunt/scheduler/block_grid.h"

#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {

using core::UInt256;
using core::ScalarInterval;

BlockGrid::BlockGrid(ScalarInterval root, UInt256 width) : root_(root), width_(width) {
    if (width_.is_zero()) throw std::invalid_argument("block width must be positive");
    const auto divided = root_.size().divmod(width_);
    // Avoid the overflowing (span + width - 1) ceiling formula.
    count_ = divided.first;
    if (!divided.second.is_zero()) count_ = count_.add(UInt256(1));
}

ScalarInterval BlockGrid::block(const UInt256& id) const {
    if (id >= count_) throw std::out_of_range("block ID outside job");
    const auto begin = root_.begin().add(id.multiply(width_));
    // Clip the span before addition, including widths larger than the job.
    const auto span = std::min(width_, root_.end().subtract(begin));
    return ScalarInterval(begin, begin.add(span));
}

UInt256 BlockGrid::block_containing(const UInt256& scalar) const {
    if (!root_.contains(scalar)) throw std::out_of_range("scalar outside job");
    return scalar.subtract(root_.begin()).divmod(width_).first;
}

} // namespace keyhunt::scheduler
