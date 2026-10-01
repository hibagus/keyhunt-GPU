#include "keyhunt/core/scalar_stride.h"
#include <stdexcept>

namespace keyhunt::core {
void validate_scalar_stride(const UInt256& stride) {
    if (stride.is_zero() || stride >= scalar_order())
        throw std::invalid_argument("scalar stride must be in [1,n)");
}
namespace {
ScalarInterval candidate_indices(const ScalarInterval& scalars, const UInt256& stride) {
    validate_scalar_stride(stride);
    if (stride == UInt256(1))
        throw std::invalid_argument("unit stride uses the original scalar mapping");
    // ceil(span/stride), without ever computing span+stride-1.
    const auto count = scalars.size().subtract(UInt256(1)).divmod(stride).first.add(UInt256(1));
    return {UInt256(1), count.add(UInt256(1))};
}
}
ScalarStride::ScalarStride(ScalarInterval scalars, UInt256 stride)
    : scalars_(scalars), stride_(stride), indices_(candidate_indices(scalars,stride)) {}
UInt256 ScalarStride::scalar(const UInt256& index) const {
    if (!indices_.contains(index)) throw std::out_of_range("candidate index outside strided range");
    return scalars_.begin().add(index.subtract(UInt256(1)).multiply(stride_));
}
UInt256 ScalarStride::index(const UInt256& scalar) const {
    if (!scalars_.contains(scalar)) throw std::out_of_range("scalar outside strided range");
    const auto offset = scalar.subtract(scalars_.begin()).divmod(stride_);
    if (!offset.second.is_zero()) throw std::invalid_argument("scalar is skipped by the stride");
    return offset.first.add(UInt256(1));
}
bool ScalarStride::operator==(const ScalarStride& other) const {
    return scalars_.begin()==other.scalars_.begin() && scalars_.end()==other.scalars_.end() && stride_==other.stride_;
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
