#pragma once
#include "keyhunt/core/xpoint_search.h"

namespace keyhunt::core {
enum class Hash160Encoding : uint8_t { Compressed=1, Uncompressed=2, Both=3 };
enum class Hash160Input { Hex, BitcoinAddress };
// The tag is part of the target identity, not an output-only display hint.
// Sorting these bytes orders encoding first, then the complete hash digest.
using Hash160Target = std::array<uint8_t,21>;
Hash160Encoding hash160_encoding(const std::string& name);
const char* hash160_encoding_name(uint8_t tag);
Hash160Target hash160_target(const UncompressedPublicKey& public_key, uint8_t tag);

class Hash160Targets {
public:
    explicit Hash160Targets(std::vector<Hash160Target> values);
    static Hash160Targets load(const std::string& path, Hash160Input input,
                               Hash160Encoding encoding=Hash160Encoding::Both);
    const std::vector<Hash160Target>& values() const { return values_; }
    const scheduler::Digest& digest() const { return digest_; }
    uint8_t encodings() const { return encodings_; }
    unsigned max_matches_per_scalar() const { return encodings_==3?2:1; }
private:
    std::vector<Hash160Target> values_;
    alignas(uint64_t) scheduler::Digest digest_{};
    uint8_t encodings_=0;
};
// Reuse the bounded scalar-offset/target record layout. Unlike full-X lookup,
// two different encoding targets may legitimately match at the same scalar.
std::vector<XPointMatch> verify_hash160(const scheduler::KernelBatch& batch,
    const Hash160Targets& targets, std::vector<XPointCandidate> candidates,
    const XPointVerifier& verifier);
} // namespace keyhunt::core
