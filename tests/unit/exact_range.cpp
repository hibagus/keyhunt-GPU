#include "keyhunt/core/exact_range.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace keyhunt::core;

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class Exception, class Function> void rejects(Function action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const Exception&) { rejected = true; }
    require(rejected, message);
}
}

int main() {
    const UInt256 zero, one(1);
    const auto max = UInt256::from_hex(std::string(64, 'f'));
    require(zero.is_zero() && !one.is_zero(), "zero predicate");
    require(one.hex() == "0x" + std::string(63, '0') + "1", "canonical fixed-width hex");
    require(UInt256::from_hex("0XAbC") == UInt256(0xabc), "hex prefix and case");
    for (const auto& text : {"", "0x", "-1", "+1", " 1", "1 ", "1\n", "0x0x1", "g", "1_0"})
        rejects<std::invalid_argument>([&] { UInt256::from_hex(text); }, "reject malformed hex");
    rejects<std::invalid_argument>([&] { UInt256::from_hex(std::string(65, '0')); }, "reject over-wide hex even with leading zeros");
    rejects<std::invalid_argument>([&] { UInt256::from_hex(std::string("1\0", 2)); }, "reject embedded NUL");
    rejects<std::overflow_error>([&] { max.add(one); }, "addition overflow");
    rejects<std::underflow_error>([&] { zero.subtract(one); }, "subtraction underflow");
    rejects<std::overflow_error>([&] { max.multiply(UInt256(2)); }, "multiplication overflow");
    rejects<std::invalid_argument>([&] { one.divmod(zero); }, "division by zero");
    require(max.multiply(one) == max && max.multiply(zero) == zero, "maximum product boundaries");
    require(max.divmod(max) == std::make_pair(one, zero), "maximum division");
    require(one.divmod(max) == std::make_pair(zero, one), "small divided by maximum");
    require(UInt256(UINT64_MAX).to_uint64() == UINT64_MAX, "maximum local index");
    rejects<std::overflow_error>([] { UInt256::power_of_two(64).to_uint64(); }, "no uint64 truncation");
    rejects<std::out_of_range>([] { UInt256::power_of_two(256); }, "no shift by 256");
    for (unsigned bit = 0; bit < 256; ++bit) {
        const auto value = UInt256::power_of_two(bit);
        const auto bytes = value.bytes();
        require(bytes[31 - bit / 8] == uint8_t(1u << (bit % 8)), "big-endian byte position");
        require(UInt256::from_bytes(bytes) == value, "byte roundtrip");
        require(UInt256::from_hex(value.hex()) == value, "hex roundtrip");
        require(value.subtract(one).add(one) == value, "carry and borrow chain");
    }
    UInt256::Bytes bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7);
    require(UInt256::from_bytes(bytes).bytes() == bytes, "mixed byte order roundtrip");
    const ScalarInterval domain(one, scalar_order());
    require(domain.contains(one), "first scalar included");
    require(domain.contains(scalar_order().subtract(one)), "last scalar included");
    require(!domain.contains(zero) && !domain.contains(scalar_order()), "exclude invalid scalars");
    require(domain.size() == scalar_order().subtract(one), "full domain count");
    const ScalarInterval tail(scalar_order().subtract(one), scalar_order());
    require(domain.contains(tail) && !tail.contains(domain), "interval containment");
    rejects<std::invalid_argument>([&] { ScalarInterval invalid(zero, one); }, "reject zero start");
    rejects<std::invalid_argument>([&] { ScalarInterval invalid(one, one); }, "reject empty range");
    rejects<std::invalid_argument>([&] { ScalarInterval invalid(UInt256(2), one); }, "reject reversed range");
    rejects<std::invalid_argument>([&] { ScalarInterval invalid(one, scalar_order().add(one)); }, "reject end above order");
    std::cout << checks << " exact range checks passed\n";
}
