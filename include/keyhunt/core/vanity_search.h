#pragma once
#include "keyhunt/core/hash160_search.h"

namespace keyhunt::core {
// Encoding, length, ASCII prefix, zero padding. Bytes are also the durable wire format.
using VanityTarget = std::array<uint8_t,36>;
VanityTarget vanity_target(const std::string& prefix,uint8_t encoding);
std::string vanity_prefix(const VanityTarget& target);
std::string bitcoin_address(const UncompressedPublicKey& public_key,uint8_t encoding);
bool vanity_matches(const std::string& address,const VanityTarget& target);
class VanityTargets {
public:
    explicit VanityTargets(std::vector<VanityTarget> values);
    static VanityTargets load(const std::string& path,Hash160Encoding encoding=Hash160Encoding::Both);
    const std::vector<VanityTarget>& values() const { return values_; }
    const scheduler::Digest& digest() const { return digest_; }
    uint64_t length_mask(uint8_t encoding) const;
    unsigned max_matches_per_scalar() const { return maximum_matches_; }
private:
    std::vector<VanityTarget> values_;
    alignas(uint64_t) scheduler::Digest digest_{};
    uint64_t lengths_[2]{};
    unsigned maximum_matches_=0;
};
std::vector<XPointMatch> verify_vanity(const scheduler::KernelBatch&,const VanityTargets&,
    std::vector<XPointCandidate>,const XPointVerifier&);
} // namespace keyhunt::core
