#pragma once
#include "common/field.h"

namespace keyhunt::gpu::test {
// Measurement-only alternative: 16x16-bit schoolbook accumulation uses 32-bit
// intermediates, then converts to the same canonical 8x32 result. Conversion is
// part of the measured cost; this is not a full alternate point representation.
KEYHUNT_HD inline void multiply16(Field& out, const Field& a, const Field& b) {
    uint16_t product[32]{};
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t left = (a.limb[i/2] >> (16*(i%2))) & 65535;
        uint32_t carry = 0;
        for (unsigned j = 0; j < 16; ++j) {
            const uint32_t right = (b.limb[j/2] >> (16*(j%2))) & 65535;
            // (2^16-1)^2 + 2*(2^16-1) fits exactly in uint32_t.
            const uint32_t sum = left*right + product[i+j] + carry;
            product[i+j] = uint16_t(sum); carry = sum >> 16;
        }
        product[i+16] = uint16_t(carry);
    }
    uint32_t words[16];
    for (unsigned i = 0; i < 16; ++i) words[i] = uint32_t(product[2*i]) | (uint32_t(product[2*i+1]) << 16);
    out = reduce_product(words);
}
}
