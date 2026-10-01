#include "keyhunt/core/scalar_stride.h"
#include <stdexcept>

namespace keyhunt::core {
void validate_scalar_stride(const UInt256& stride) {
    if (stride.is_zero() || stride >= scalar_order())
        throw std::invalid_argument("scalar stride must be in [1,n)");
}
namespace {
UInt256 count_seeds(const ScalarInterval& scalars,const UInt256& stride,bool reverse,bool orbit) {
    validate_scalar_stride(stride);
    if (stride==UInt256(1) && !reverse && !orbit)
        throw std::invalid_argument("unit stride uses the original scalar mapping");
    return scalars.size().subtract(UInt256(1)).divmod(stride).first.add(UInt256(1));
}
ScalarInterval candidate_indices(const UInt256& count,bool orbit) {
    // Check the journal's coordinate bound before multiplying. This also prevents
    // an expanded count from overflowing the underlying 256-bit integer.
    if(orbit && count>scalar_order().subtract(UInt256(1)).divmod(UInt256(6)).first)
        throw std::invalid_argument("orbit expansion exceeds candidate-index limit; split the seed range into smaller jobs");
    return {UInt256(1),(orbit?count.multiply(UInt256(6)):count).add(UInt256(1))};
}
UInt256 add_mod(const UInt256& a,const UInt256& b) {
    const auto gap=scalar_order().subtract(b);
    return a>=gap?a.subtract(gap):a.add(b);
}
}
UInt256 scalar_orbit(const UInt256& seed,unsigned variant) {
    validate_scalar_stride(seed);
    if(variant>=6)throw std::invalid_argument("orbit variant must be in [0,5]");
    UInt256 value=seed;
    if(variant>=2){
        // Integer multiplication modulo n, independent of the device's field
        // transformation modulo p. Comparison before addition avoids overflow.
        const auto factor=UInt256::from_hex(variant<4?
            "5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72":
            "ac9c52b33fa3cf1f5ad9e3fd77ed9ba4a880b9fc8ec739c2e0cfc810b51283ce");
        value=UInt256();
        for(const auto byte:factor.bytes())for(int bit=7;bit>=0;--bit){
            value=add_mod(value,value);
            if((byte>>bit)&1U)value=add_mod(value,seed);
        }
    }
    return variant%2?scalar_order().subtract(value):value;
}
ScalarStride::ScalarStride(ScalarInterval scalars, UInt256 stride, bool reverse, bool orbit)
    : scalars_(scalars), stride_(stride), seed_count_(count_seeds(scalars,stride,reverse,orbit)),
      indices_(candidate_indices(seed_count_,orbit)), reverse_(reverse), orbit_(orbit) {}
unsigned ScalarStride::variant(const UInt256& index) const {
    if(!indices_.contains(index))throw std::out_of_range("candidate index outside mapped range");
    return orbit_?unsigned(index.subtract(UInt256(1)).divmod(seed_count_).first.to_uint64()):0;
}
UInt256 ScalarStride::variant_end(const UInt256& index) const {
    const auto member=variant(index);
    return orbit_?seed_count_.multiply(UInt256(member+1)).add(UInt256(1)):indices_.end();
}
UInt256 ScalarStride::seed(const UInt256& index) const {
    if(!indices_.contains(index))throw std::out_of_range("candidate index outside mapped range");
    auto offset=index.subtract(UInt256(1));
    if(orbit_)offset=offset.divmod(seed_count_).second;
    if(reverse_)offset=seed_count_.subtract(UInt256(1)).subtract(offset);
    return scalars_.begin().add(offset.multiply(stride_));
}
UInt256 ScalarStride::scalar(const UInt256& index) const {
    const auto value=seed(index);
    return orbit_?scalar_orbit(value,variant(index)):value;
}
UInt256 ScalarStride::index(const UInt256& scalar) const {
    if(orbit_)throw std::logic_error("orbit inverse requires seed and variant");
    return index(scalar,0);
}
UInt256 ScalarStride::index(const UInt256& seed,unsigned variant) const {
    if(variant>=(orbit_?6U:1U))throw std::invalid_argument("invalid mapping variant");
    if(!scalars_.contains(seed))throw std::out_of_range("seed outside mapped range");
    const auto offset=seed.subtract(scalars_.begin()).divmod(stride_);
    if(!offset.second.is_zero())throw std::invalid_argument("seed is skipped by the stride");
    const auto index=reverse_?seed_count_.subtract(offset.first):offset.first.add(UInt256(1));
    return index.add(seed_count_.multiply(UInt256(variant)));
}
bool ScalarStride::operator==(const ScalarStride& other) const {
    return scalars_.begin()==other.scalars_.begin() && scalars_.end()==other.scalars_.end() &&
        stride_==other.stride_ && reverse_==other.reverse_ && orbit_==other.orbit_;
}
UInt256 scalar_stride_power(UInt256 stride, unsigned bit) {
    validate_scalar_stride(stride);
    if (bit>255) throw std::invalid_argument("stride power exceeds 255 bits");
    while (bit--) {
        const auto complement=scalar_order().subtract(stride);
        stride = stride>=complement ? stride.subtract(complement) : stride.add(stride);
    }
    return stride;
}
} // namespace keyhunt::core
