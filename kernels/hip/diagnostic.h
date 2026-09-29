#pragma once
#include <hip/hip_runtime.h>
#include <cstdint>

namespace keyhunt::backend {
struct DiagnosticScalar { uint8_t bytes[32]; };

// A deliberately simple transport/index diagnostic, not secp256k1 arithmetic.
// Host planning proves that every begin + index fits the scalar interval.
__global__ void diagnostic_indices(DiagnosticScalar begin, uint64_t count,
                                  DiagnosticScalar* output, unsigned long long* executed) {
    const uint64_t index = uint64_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count) return; // partial workgroups must not touch the guard slot
    DiagnosticScalar scalar{};
    uint64_t carry = index;
    for (int byte = 31; byte >= 0; --byte) {
        const unsigned sum = begin.bytes[byte] + unsigned(carry & 255);
        scalar.bytes[byte] = static_cast<uint8_t>(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
    output[index] = scalar;
    atomicAdd(executed, 1ULL); // proves actual device work, not just host submission
}
}
