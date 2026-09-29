#pragma once

#include <array>
#include <cstdint>

class Secp256K1;

namespace keyhunt::core {

// Fixed-width network (big-endian) bytes, independent of Int and device layouts.
using ScalarBytes = std::array<uint8_t, 32>;
using XPointBytes = std::array<uint8_t, 32>;
using Hash160Bytes = std::array<uint8_t, 20>;
using CompressedPublicKey = std::array<uint8_t, 33>;
using UncompressedPublicKey = std::array<uint8_t, 65>;

// The caller initializes the CPU curve once before starting workers and keeps it
// alive. Verification uses per-call temporaries and does not initialize the curve.
// A match proves only the target relation: range membership, execution identity,
// deduplication, durable output and coverage belong to subsequent host stages.
class CpuResultVerifier {
public:
    explicit CpuResultVerifier(Secp256K1& curve) : curve(curve) {}

    // Rejects 0 and values >= the curve order; never silently reduces a scalar.
    // On failure, the output is unchanged.
    bool derive(const ScalarBytes& scalar, UncompressedPublicKey& output) const;
    bool matches_xpoint(const ScalarBytes& scalar, const XPointBytes& target) const;
    bool matches_public_key(const ScalarBytes& scalar, const CompressedPublicKey& target) const;
    bool matches_public_key(const ScalarBytes& scalar, const UncompressedPublicKey& target) const;
    bool matches_hash160(const ScalarBytes& scalar, const Hash160Bytes& target, bool compressed) const;
    bool matches_ethereum(const ScalarBytes& scalar, const Hash160Bytes& target) const;

private:
    Secp256K1& curve;
};

} // namespace keyhunt::core
