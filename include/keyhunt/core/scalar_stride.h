#pragma once
#include "keyhunt/core/exact_range.h"

namespace keyhunt::core {
void validate_scalar_stride(const UInt256& stride);
// Non-unit forward strides and all reverse traversals use candidate indices. The
// scalar range remains immutable mapping metadata, never a coverage interval.
class ScalarStride {
public:
    ScalarStride(ScalarInterval scalars, UInt256 stride, bool reverse=false);
    const ScalarInterval& scalars() const { return scalars_; }
    const UInt256& stride() const { return stride_; }
    const ScalarInterval& indices() const { return indices_; }
    bool reverse() const { return reverse_; }
    const char* coordinate_space() const { return reverse_?"scalar-reverse-index-v1":"scalar-stride-index-v1"; }
    UInt256 scalar(const UInt256& index) const;
    UInt256 index(const UInt256& scalar) const;
    bool operator==(const ScalarStride& other) const;
    bool operator!=(const ScalarStride& other) const { return !(*this == other); }
private:
    ScalarInterval scalars_;
    UInt256 stride_;
    ScalarInterval indices_;
    bool reverse_;
};
// Cached point offsets need S*2^bit modulo n, unlike candidate enumeration.
// Doubling by comparison with n-x avoids overflowing a 256-bit intermediate.
UInt256 scalar_stride_power(UInt256 stride, unsigned bit);
} // namespace keyhunt::core
