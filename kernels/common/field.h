#pragma once
#include <cstdint>

// Shared integer arithmetic only: no HIP runtime, warp assumptions or ISA code.
#if defined(__HIPCC__) || defined(__CUDACC__)
#define KEYHUNT_HD __host__ __device__
#else
#define KEYHUNT_HD
#endif

namespace keyhunt::gpu {
// Canonical field element: 0 <= value < p, little-endian radix-2^32 limbs.
// Raw limbs are an internal representation, never a serialized point/scalar.
struct Field { uint32_t limb[8]{}; };
KEYHUNT_HD inline Field prime() {
    return {{0xfffffc2fU, 0xfffffffeU, 0xffffffffU, 0xffffffffU,
             0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU}};
}
KEYHUNT_HD inline Field one() { return {{1,0,0,0,0,0,0,0}}; }
KEYHUNT_HD inline bool equal(const Field& a, const Field& b) {
    uint32_t different = 0;
    for (unsigned i = 0; i < 8; ++i) different |= a.limb[i] ^ b.limb[i];
    return different == 0;
}
KEYHUNT_HD inline bool is_zero(const Field& a) { return equal(a, Field{}); }
KEYHUNT_HD inline bool less(const Field& a, const Field& b) {
    for (int i = 7; i >= 0; --i)
        if (a.limb[i] != b.limb[i]) return a.limb[i] < b.limb[i];
    return false;
}
KEYHUNT_HD inline Field subtract_words(const Field& a, const Field& b, uint32_t& borrow) {
    Field result{};
    borrow = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint64_t rhs = uint64_t(b.limb[i]) + borrow;
        result.limb[i] = uint32_t(uint64_t(a.limb[i]) - rhs);
        borrow = uint64_t(a.limb[i]) < rhs;
    }
    return result;
}
// Any 256-bit integer is below 2p, so one subtraction suffices at ingress.
KEYHUNT_HD inline Field normalize(Field a) {
    const auto p = prime();
    if (!less(a, p)) { uint32_t borrow; a = subtract_words(a, p, borrow); }
    return a;
}
KEYHUNT_HD inline Field from_bytes_reduced(const uint8_t bytes[32]) {
    Field value{};
    for (unsigned i = 0; i < 32; ++i)
        value.limb[(31-i)/4] |= uint32_t(bytes[i]) << (8*((31-i)%4));
    return normalize(value);
}
KEYHUNT_HD inline bool from_bytes_checked(Field& out, const uint8_t bytes[32]) {
    Field value{};
    for (unsigned i = 0; i < 32; ++i)
        value.limb[(31-i)/4] |= uint32_t(bytes[i]) << (8*((31-i)%4));
    if (!less(value, prime())) { out = Field{}; return false; }
    out = value;
    return true;
}
KEYHUNT_HD inline void to_bytes(uint8_t bytes[32], const Field& value) {
    for (unsigned i = 0; i < 32; ++i)
        bytes[i] = uint8_t(value.limb[(31-i)/4] >> (8*((31-i)%4)));
}

// Reduce low + high*2^256, using 2^256 == 2^32 + 977 (mod p).
// Multiplication supplies high <= 2^32+977. Its first fold adds <2^66;
// if that carries out of bit 255, the next fold cannot carry again. No 33-bit
// constant is multiplied by a full 32-bit limb in an overflowing 64-bit sum.
KEYHUNT_HD inline Field fold(Field low, uint64_t high) {
    while (high) {
        uint64_t carry = high * 977;
        for (unsigned i = 0; i < 8; ++i) {
            const uint64_t shifted = i == 1 ? uint32_t(high) : (i == 2 ? high >> 32 : 0);
            const uint64_t sum = uint64_t(low.limb[i]) + carry + shifted;
            low.limb[i] = uint32_t(sum);
            carry = sum >> 32;
        }
        high = carry;
    }
    return normalize(low);
}
KEYHUNT_HD inline void add(Field& out, const Field& a, const Field& b) {
    Field result{};
    uint64_t carry = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint64_t sum = uint64_t(a.limb[i]) + b.limb[i] + carry;
        result.limb[i] = uint32_t(sum);
        carry = sum >> 32;
    }
    out = fold(result, carry);
}
KEYHUNT_HD inline void sub(Field& out, const Field& a, const Field& b) {
    uint32_t borrow;
    Field result = subtract_words(a, b, borrow);
    if (borrow) {
        const auto p = prime();
        uint64_t carry = 0;
        for (unsigned i = 0; i < 8; ++i) {
            const uint64_t sum = uint64_t(result.limb[i]) + p.limb[i] + carry;
            result.limb[i] = uint32_t(sum);
            carry = sum >> 32;
        }
    }
    out = result;
}
KEYHUNT_HD inline void neg(Field& out, const Field& a) { sub(out, Field{}, a); }
// The reduction is independent of how the full product was accumulated. Keeping
// it separate also lets test-only radix alternatives share the proven fold.
KEYHUNT_HD inline Field reduce_product(const uint32_t product[16]) {
    Field low{};
    uint64_t carry = 0;
    for (unsigned i = 0; i < 8; ++i) {
        // Fold the upper eight limbs without dropping the shifted top limb.
        const uint64_t sum = uint64_t(product[i]) + uint64_t(product[i+8])*977
                           + (i ? product[i+7] : 0) + carry;
        low.limb[i] = uint32_t(sum);
        carry = sum >> 32;
    }
    return fold(low, carry + product[15]);
}
KEYHUNT_HD inline void mul(Field& out, const Field& a, const Field& b) {
    uint32_t product[16]{};
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < 8; ++j) {
            // (B-1)^2 + (B-1) + (B-1) = B^2-1, with B=2^32.
            const uint64_t sum = uint64_t(a.limb[i]) * b.limb[j] + product[i+j] + carry;
            product[i+j] = uint32_t(sum);
            carry = sum >> 32;
        }
        product[i+8] = uint32_t(carry);
    }
    out = reduce_product(product);
}
KEYHUNT_HD inline void square(Field& out, const Field& a) { mul(out, a, a); }
KEYHUNT_HD inline bool inverse(Field& out, const Field& a) {
    // Fermat inversion is a correctness baseline. Zero has no inverse; return
    // false and an explicit zero, including when out aliases a.
    if (is_zero(a)) { out = Field{}; return false; }
    Field exponent = prime();
    exponent.limb[0] -= 2;
    Field result = one();
    for (int bit = 255; bit >= 0; --bit) {
        square(result, result);
        if ((exponent.limb[bit/32] >> (bit%32)) & 1) mul(result, result, a);
    }
    out = result;
    return true;
}

// One serial group per caller. Exact in-place operation is supported; partially
// overlapping arrays are not. Zero lanes map to zero and never enter the product.
// The fixed template capacity bounds thread-local scratch for future kernels.
template<unsigned Capacity>
KEYHUNT_HD bool batch_inverse(Field* out, const Field* in, unsigned count) {
    static_assert(Capacity > 0, "batch inversion needs bounded scratch");
    if (count > Capacity) return false; // reject before touching output
    Field prefix[Capacity];
    Field product = one();
    for (unsigned i = 0; i < count; ++i) {
        prefix[i] = product;
        if (!is_zero(in[i])) mul(product, product, in[i]);
    }
    Field reciprocal;
    inverse(reciprocal, product);
    for (unsigned i = count; i-- > 0;) {
        const Field value = in[i]; // preserve the input before an aliased write
        if (is_zero(value)) out[i] = Field{};
        else {
            mul(out[i], reciprocal, prefix[i]);
            mul(reciprocal, reciprocal, value);
        }
    }
    return true;
}
} // namespace keyhunt::gpu
