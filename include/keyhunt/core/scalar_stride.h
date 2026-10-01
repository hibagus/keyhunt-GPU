#pragma once
#include "keyhunt/core/exact_range.h"

namespace keyhunt::core {
void validate_scalar_stride(const UInt256& stride);
// Non-unit forward strides and all reverse traversals use candidate indices. The
// scalar range remains immutable mapping metadata, never a coverage interval.
class ScalarStride {
public:
    ScalarStride(ScalarInterval scalars, UInt256 stride, bool reverse=false, bool orbit=false);
    const ScalarInterval& scalars() const { return scalars_; }
    const UInt256& stride() const { return stride_; }
    const ScalarInterval& indices() const { return indices_; }
    bool reverse() const { return reverse_; }
    bool orbit() const { return orbit_; }
    const UInt256& seed_count() const { return seed_count_; }
    UInt256 seed(const UInt256& index) const;
    unsigned variant(const UInt256& index) const;
    UInt256 variant_end(const UInt256& index) const;
    // An expanded orbit is not injective across seeds. Its inverse needs the
    // original seed and variant, rather than just the resulting private scalar.
    UInt256 index(const UInt256& seed, unsigned variant) const;
    const char* coordinate_space() const { return orbit_?"scalar-orbit-index-v1":reverse_?"scalar-reverse-index-v1":"scalar-stride-index-v1"; }
    UInt256 scalar(const UInt256& index) const;
    UInt256 index(const UInt256& scalar) const;
    bool operator==(const ScalarStride& other) const;
    bool operator!=(const ScalarStride& other) const { return !(*this == other); }
private:
    ScalarInterval scalars_;
    UInt256 stride_;
    UInt256 seed_count_;
    ScalarInterval indices_;
    bool reverse_, orbit_;
};
// Map a nonzero seed to one of k, -k, lambda*k, -lambda*k, lambda²*k, -lambda²*k.
UInt256 scalar_orbit(const UInt256& seed, unsigned variant);
// Cached point offsets need S*2^bit modulo n, unlike candidate enumeration.
// Doubling by comparison with n-x avoids overflowing a 256-bit intermediate.
UInt256 scalar_stride_power(UInt256 stride, unsigned bit);
} // namespace keyhunt::core
