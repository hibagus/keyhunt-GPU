#pragma once
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::core {
using EthereumTarget = std::array<uint8_t,20>;
// Ethereum hashes the full affine X || Y, without a SEC1 prefix.
EthereumTarget ethereum_target(const UncompressedPublicKey& public_key);
EthereumTarget parse_ethereum_address(const std::string& text);

class EthereumTargets {
public:
    explicit EthereumTargets(std::vector<EthereumTarget> values);
    static EthereumTargets load(const std::string& path);
    const std::vector<EthereumTarget>& values() const { return values_; }
    const scheduler::Digest& digest() const { return digest_; }
private:
    std::vector<EthereumTarget> values_;
    alignas(uint64_t) scheduler::Digest digest_{};
};
std::vector<XPointMatch> verify_ethereum(const scheduler::KernelBatch& batch,
    const EthereumTargets& targets, std::vector<XPointCandidate> candidates,
    const XPointVerifier& verifier);
} // namespace keyhunt::core
