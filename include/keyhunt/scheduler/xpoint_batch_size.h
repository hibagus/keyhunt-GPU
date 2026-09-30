#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace keyhunt::scheduler {
// Xpoint emits at most one candidate per scalar. After overflow, a batch no
// larger than the buffer is guaranteed to fit, even for fully dense targets.
// Spare output capacity lets later sparse work recover exponentially instead
// of retaining that small launch size for the rest of the assignment.
class XPointBatchSize {
public:
    XPointBatchSize(uint64_t maximum,uint32_t capacity)
        : maximum_(maximum),limit_(maximum),capacity_(capacity) {
        if (!maximum || maximum>1048576 || !capacity)
            throw std::invalid_argument("invalid xpoint batch sizing bounds");
    }
    uint64_t limit() const { return limit_; }
    void overflow(uint64_t steps) {
        if (steps<2) throw std::logic_error("single xpoint step cannot overflow a nonempty buffer");
        limit_=std::min<uint64_t>(capacity_,steps/2);
    }
    void accepted(uint64_t candidates) {
        // Keep full buffers stable on dense inputs. A half-full or emptier
        // buffer permits one bounded doubling; maximum_ is at most 2^20.
        if (candidates<=capacity_/2) limit_=std::min(maximum_,2*limit_);
    }
private:
    uint64_t maximum_,limit_;
    uint32_t capacity_;
};
} // namespace keyhunt::scheduler
