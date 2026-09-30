#pragma once
#include "keyhunt/core/exact_range.h"
#include <algorithm>
#include <stdexcept>

namespace keyhunt::scheduler {
// Estimate only the next work unit. Exact scalar bounds and durable coverage
// remain integer quantities; changing this estimate cannot resize a leased block.
class AdaptiveWorkSize {
public:
    AdaptiveWorkSize(core::UInt256 initial,unsigned seconds,uint64_t alignment=1)
        :span_(initial),seconds_(seconds),alignment_(alignment){
        if(initial.is_zero()||seconds>300||!alignment)throw std::invalid_argument("invalid adaptive work geometry");
    }
    const core::UInt256& span() const{return span_;}
    void observed(const core::UInt256& covered,uint64_t active_ns){
        if(!seconds_||!active_ns||covered.is_zero())return;
        using core::UInt256;
        const UInt256 target(uint64_t(seconds_)*1000000000),elapsed(active_ns),alignment(alignment_);
        // Divide before multiplying the wide quotient. The remainder product
        // fits 104 bits; overflow of the quotient means the final span is huge.
        const auto parts=covered.divmod(elapsed);
        try{
            span_=parts.first.multiply(target).add(parts.second.multiply(target).divmod(elapsed).first);
        }catch(const std::overflow_error&){span_=core::scalar_order().subtract(UInt256(1));}
        span_=std::max(alignment,span_.divmod(alignment).first.multiply(alignment));
    }
private:
    core::UInt256 span_;
    unsigned seconds_;
    uint64_t alignment_;
};
}
