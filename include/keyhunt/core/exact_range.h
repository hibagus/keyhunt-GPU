#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace keyhunt::core {

// Host integer for scalars, endpoints, widths, counts and IDs. Arithmetic is
// checked integer arithmetic, never arithmetic modulo the curve order.
class UInt256 {
public:
    using Bytes = std::array<uint8_t, 32>;

    explicit UInt256(uint64_t value = 0);
    static UInt256 from_hex(std::string_view text);
    static UInt256 from_bytes(const Bytes& bytes);
    static UInt256 power_of_two(unsigned bit); // 0 <= bit < 256
    Bytes bytes() const;
    std::string hex() const; // 0x followed by exactly 64 lowercase digits
    uint64_t to_uint64() const; // throws if it would truncate
    bool is_zero() const;

    UInt256 add(const UInt256& rhs) const;
    UInt256 subtract(const UInt256& rhs) const;
    UInt256 multiply(const UInt256& rhs) const;
    std::pair<UInt256, UInt256> divmod(const UInt256& divisor) const;

    friend bool operator==(const UInt256& a, const UInt256& b) { return a.limbs_ == b.limbs_; }
    friend bool operator!=(const UInt256& a, const UInt256& b) { return !(a == b); }
    friend bool operator<(const UInt256& a, const UInt256& b);
    friend bool operator>(const UInt256& a, const UInt256& b) { return b < a; }
    friend bool operator<=(const UInt256& a, const UInt256& b) { return !(b < a); }
    friend bool operator>=(const UInt256& a, const UInt256& b) { return !(a < b); }

private:
    std::array<uint32_t, 8> limbs_{}; // little-endian limbs; not a storage format
};

const UInt256& scalar_order();

// Nonempty half-open scalar interval: 1 <= begin < end <= secp256k1 order.
// The exclusive endpoint may equal the order; an actual scalar may not.
class ScalarInterval {
public:
    ScalarInterval(UInt256 begin, UInt256 end);
    const UInt256& begin() const { return begin_; }
    const UInt256& end() const { return end_; }
    UInt256 size() const { return end_.subtract(begin_); }
    bool contains(const UInt256& scalar) const { return begin_ <= scalar && scalar < end_; }
    bool contains(const ScalarInterval& other) const {
        return begin_ <= other.begin_ && other.end_ <= end_;
    }

private:
    UInt256 begin_, end_;
};

} // namespace keyhunt::core
