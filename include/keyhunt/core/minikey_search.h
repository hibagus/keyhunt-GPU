#pragma once
#include "keyhunt/core/hash160_search.h"

namespace keyhunt::core {
// Length is repeated in each canonical target so the existing immutable target
// binding also defines the ordinal space. Mixed lengths within a job are invalid.
using MinikeyTarget=std::array<uint8_t,22>;
bool parse_minikey_order(const std::string& value); // true means reverse execution, not a scalar mapping
UInt256 minikey_space_end(unsigned length);
std::string minikey_text(const UInt256& ordinal,unsigned length);
UInt256 minikey_ordinal(const std::string& text);
std::optional<UInt256> minikey_scalar(const std::string& text);
MinikeyTarget minikey_target(unsigned length,const Hash160Target& target);
class MinikeyTargets {
public:
    explicit MinikeyTargets(std::vector<MinikeyTarget> values);
    static MinikeyTargets load(const std::string&,unsigned length,Hash160Input=Hash160Input::BitcoinAddress,
                               Hash160Encoding=Hash160Encoding::Both);
    const std::vector<MinikeyTarget>& values()const{return values_;}
    const scheduler::Digest& digest()const{return digest_;}
    unsigned length()const{return values_[0][0];}
    uint8_t encodings()const{return encodings_;}
    unsigned max_matches_per_scalar()const{return encodings_==3?2:1;}
    void validate_interval(const ScalarInterval&)const;
private:
    std::vector<MinikeyTarget> values_;
    alignas(uint64_t) scheduler::Digest digest_{};
    uint8_t encodings_=0;
};
// Receipt coordinates are ordinals, not derived private scalars. Retaining an
// ordinal lets every verifier reconstruct the exact candidate without trusting GPU text.
std::vector<XPointMatch> verify_minikeys(const scheduler::KernelBatch&,const MinikeyTargets&,
    std::vector<XPointCandidate>,const XPointVerifier&);
} // namespace keyhunt::core
