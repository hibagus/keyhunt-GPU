#include "keyhunt/core/exact_range.h"

#include <stdexcept>

namespace keyhunt::core {

UInt256::UInt256(uint64_t value) {
    limbs_[0] = static_cast<uint32_t>(value);
    limbs_[1] = static_cast<uint32_t>(value >> 32);
}

UInt256 UInt256::from_hex(std::string_view text) {
    if (text.substr(0, 2) == "0x" || text.substr(0, 2) == "0X") text.remove_prefix(2);
    if (text.empty() || text.size() > 64) throw std::invalid_argument("expected 1 to 64 hex digits");
    UInt256 result;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[text.size() - 1 - i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else throw std::invalid_argument("invalid hex digit");
        result.limbs_[i / 8] |= digit << (4 * (i % 8));
    }
    return result;
}

UInt256 UInt256::from_bytes(const Bytes& bytes) {
    UInt256 result;
    for (size_t i = 0; i < bytes.size(); ++i)
        result.limbs_[i / 4] |= uint32_t(bytes[31 - i]) << (8 * (i % 4));
    return result;
}

UInt256 UInt256::power_of_two(unsigned bit) {
    if (bit >= 256) throw std::out_of_range("power of two exceeds UInt256");
    UInt256 result;
    result.limbs_[bit / 32] = uint32_t{1} << (bit % 32);
    return result;
}

UInt256::Bytes UInt256::bytes() const {
    Bytes result{};
    for (size_t i = 0; i < result.size(); ++i)
        result[31 - i] = static_cast<uint8_t>(limbs_[i / 4] >> (8 * (i % 4)));
    return result;
}

std::string UInt256::hex() const {
    constexpr char digits[] = "0123456789abcdef";
    const auto value = bytes();
    std::string result = "0x";
    result.reserve(66);
    for (uint8_t byte : value) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

uint64_t UInt256::to_uint64() const {
    for (size_t i = 2; i < limbs_.size(); ++i)
        if (limbs_[i]) throw std::overflow_error("UInt256 does not fit uint64_t");
    return uint64_t(limbs_[0]) | (uint64_t(limbs_[1]) << 32);
}

bool UInt256::is_zero() const {
    for (uint32_t limb : limbs_) if (limb) return false;
    return true;
}

bool operator<(const UInt256& a, const UInt256& b) {
    for (size_t i = a.limbs_.size(); i-- > 0;)
        if (a.limbs_[i] != b.limbs_[i]) return a.limbs_[i] < b.limbs_[i];
    return false;
}

UInt256 UInt256::add(const UInt256& rhs) const {
    UInt256 result;
    uint64_t carry = 0;
    for (size_t i = 0; i < limbs_.size(); ++i) {
        const uint64_t sum = uint64_t(limbs_[i]) + rhs.limbs_[i] + carry;
        result.limbs_[i] = static_cast<uint32_t>(sum);
        carry = sum >> 32;
    }
    if (carry) throw std::overflow_error("UInt256 addition overflow");
    return result;
}

UInt256 UInt256::subtract(const UInt256& rhs) const {
    UInt256 result;
    uint64_t borrow = 0;
    for (size_t i = 0; i < limbs_.size(); ++i) {
        const uint64_t subtrahend = uint64_t(rhs.limbs_[i]) + borrow;
        result.limbs_[i] = static_cast<uint32_t>(uint64_t(limbs_[i]) - subtrahend);
        borrow = uint64_t(limbs_[i]) < subtrahend;
    }
    if (borrow) throw std::underflow_error("UInt256 subtraction underflow");
    return result;
}

UInt256 UInt256::multiply(const UInt256& rhs) const {
    // Full 512-bit product. Each multiply/add/carry fits an unsigned 64-bit
    // temporary: (2^32-1)^2 + 2*(2^32-1) == 2^64-1.
    std::array<uint32_t, 16> product{};
    for (size_t i = 0; i < limbs_.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < rhs.limbs_.size(); ++j) {
            const uint64_t value = uint64_t(limbs_[i]) * rhs.limbs_[j] + product[i+j] + carry;
            product[i+j] = static_cast<uint32_t>(value);
            carry = value >> 32;
        }
        product[i+8] = static_cast<uint32_t>(carry);
    }
    for (size_t i = 8; i < product.size(); ++i)
        if (product[i]) throw std::overflow_error("UInt256 multiplication overflow");
    UInt256 result;
    for (size_t i = 0; i < result.limbs_.size(); ++i) result.limbs_[i] = product[i];
    return result;
}

std::pair<UInt256, UInt256> UInt256::divmod(const UInt256& divisor) const {
    if (divisor.is_zero()) throw std::invalid_argument("division by zero");
    UInt256 quotient, remainder;
    // Binary long division retains the 257th remainder bit explicitly. This
    // also handles a divisor with its top bit set without wrapping a shift.
    for (unsigned bit = 256; bit-- > 0;) {
        uint32_t carry = (limbs_[bit / 32] >> (bit % 32)) & 1;
        for (auto& limb : remainder.limbs_) {
            const uint32_t next = limb >> 31;
            limb = (limb << 1) | carry;
            carry = next;
        }
        if (carry || remainder >= divisor) {
            uint64_t borrow = 0;
            for (size_t i = 0; i < remainder.limbs_.size(); ++i) {
                const uint64_t subtrahend = uint64_t(divisor.limbs_[i]) + borrow;
                const uint32_t original = remainder.limbs_[i];
                remainder.limbs_[i] = static_cast<uint32_t>(uint64_t(original) - subtrahend);
                borrow = uint64_t(original) < subtrahend;
            }
            // The subtraction consumes the high carry, if present. The
            // invariant remainder < divisor guarantees the result fits.
            quotient.limbs_[bit / 32] |= uint32_t{1} << (bit % 32);
        }
    }
    return {quotient, remainder};
}

const UInt256& scalar_order() {
    static const UInt256 order = UInt256::from_hex(
        "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    return order;
}

ScalarInterval::ScalarInterval(UInt256 begin, UInt256 end) : begin_(begin), end_(end) {
    if (begin_.is_zero() || begin_ >= end_ || end_ > scalar_order())
        throw std::invalid_argument("scalar interval requires 1 <= begin < end <= order");
}

} // namespace keyhunt::core
