#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace keyhunt::scheduler {
// Scalar searches emit a bounded number of candidates per scalar (one for
// xpoint, up to two for HASH160). Replay divides capacity by that bound.
// Spare output capacity lets later sparse work recover exponentially instead
// of retaining that small launch size for the rest of the assignment.
class XPointBatchSize {
public:
    XPointBatchSize(uint64_t maximum,uint32_t capacity,unsigned matches_per_scalar=1)
        : maximum_(maximum),limit_(maximum),capacity_(capacity),matches_per_scalar_(matches_per_scalar) {
        if (!maximum || maximum>1048576 || !matches_per_scalar || capacity<matches_per_scalar)
            throw std::invalid_argument("invalid xpoint batch sizing bounds");
    }
    uint64_t limit() const { return limit_; }
    void overflow(uint64_t steps) {
        if (steps<2) throw std::logic_error("single scalar step cannot overflow a nonempty buffer");
        limit_=std::min<uint64_t>(capacity_/matches_per_scalar_,steps/2);
    }
    void accepted(uint64_t candidates) {
        // Keep full buffers stable on dense inputs. A half-full or emptier
        // buffer permits one bounded doubling; maximum_ is at most 2^20.
        if (candidates<=capacity_/2) limit_=std::min(maximum_,2*limit_);
    }
private:
    uint64_t maximum_,limit_;
    uint32_t capacity_;
    unsigned matches_per_scalar_;
};
} // namespace keyhunt::scheduler
