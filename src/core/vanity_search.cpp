#include "keyhunt/core/vanity_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "base58/libbase58.h"
#include <algorithm>
#include <fstream>
#include <tuple>

namespace keyhunt::core {
VanityTarget vanity_target(const std::string& prefix,uint8_t encoding) {
    hash160_encoding_name(encoding);
    const std::string alphabet="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    if(prefix.empty() || prefix.size()>34 || prefix[0]!='1' || prefix.find_first_not_of(alphabet)!=std::string::npos)
        throw std::invalid_argument("vanity prefix must have 1..34 Bitcoin Base58 characters and begin with 1");
    VanityTarget target{};target[0]=encoding;target[1]=uint8_t(prefix.size());
    std::copy(prefix.begin(),prefix.end(),target.begin()+2);
    return target;
}
std::string vanity_prefix(const VanityTarget& target) {
    if(target[1]<1 || target[1]>34)throw std::invalid_argument("invalid vanity prefix length");
    return std::string(target.begin()+2,target.begin()+2+target[1]);
}
std::string bitcoin_address(const UncompressedPublicKey& public_key,uint8_t encoding) {
    const auto hash=hash160_target(public_key,encoding);
    alignas(uint64_t) uint8_t payload[25]{},first[32],second[32];
    std::copy(hash.begin()+1,hash.end(),payload+1);
    sha256(payload,21,first);sha256(first,32,second);std::copy_n(second,4,payload+21);
    char address[36];size_t length=sizeof(address);
    if(!b58enc(address,&length,payload,sizeof(payload)))throw std::runtime_error("CPU Base58Check encoding failed");
    return address;
}
bool vanity_matches(const std::string& address,const VanityTarget& target) {
    const auto prefix=vanity_prefix(target);
    return address.compare(0,prefix.size(),prefix)==0;
}
VanityTargets::VanityTargets(std::vector<VanityTarget> values):values_(std::move(values)) {
    if(values_.empty() || values_.size()>4096)throw std::invalid_argument("vanity target count must be in [1, 4096], including encoding tags");
    for(const auto& target:values_) {
        // Reconstructing catches nonzero padding as well as invalid tags/text.
        if(vanity_target(vanity_prefix(target),target[0])!=target)throw std::invalid_argument("noncanonical vanity target");
        lengths_[target[0]-1]|=uint64_t(1)<<target[1];
    }
    // Only one distinct prefix per length can match one address. This bound
    // permits thousands of disjoint prefixes without requiring thousands of slots.
    for(auto mask:lengths_)while(mask){maximum_matches_+=unsigned(mask&1);mask>>=1;}
    std::sort(values_.begin(),values_.end());values_.erase(std::unique(values_.begin(),values_.end()),values_.end());
    std::vector<uint8_t> encoded{'v','a','n','i','t','y','-','p','2','p','k','h','-','v','1',0};
    for(const auto& target:values_)encoded.insert(encoded.end(),target.begin(),target.end());
    sha256(encoded.data(),encoded.size(),digest_.data());
}
uint64_t VanityTargets::length_mask(uint8_t encoding) const {
    hash160_encoding_name(encoding);return lengths_[encoding-1];
}
VanityTargets VanityTargets::load(const std::string& path,Hash160Encoding encoding) {
    const auto mask=uint8_t(encoding);
    if(mask<1 || mask>3)throw std::invalid_argument("invalid vanity encoding selection");
    std::ifstream stream(path);if(!stream)throw std::runtime_error("cannot open vanity prefixes: "+path);
    std::vector<VanityTarget> values;char line[37];
    while(stream.getline(line,sizeof(line))) {
        const size_t length=size_t(stream.gcount())-(stream.eof()?0:1);
        std::string text(line,length);if(!text.empty()&&text.back()=='\r')text.pop_back();
        if(text.empty())continue;
        for(uint8_t tag:{1,2})if(mask&tag){
            if(values.size()==4096)throw std::invalid_argument("too many vanity prefixes");
            values.push_back(vanity_target(text,tag));
        }
    }
    if(stream.bad()||!stream.eof())throw std::runtime_error("failed to read vanity file or line too long");
    return VanityTargets(std::move(values));
}
std::vector<XPointMatch> verify_vanity(const scheduler::KernelBatch& batch,const VanityTargets& targets,
    std::vector<XPointCandidate> candidates,const XPointVerifier& verifier) {
    if(batch.work().identity().algorithm!=scheduler::WorkAlgorithm::DirectVanityV1 ||
       batch.work().identity().target_digest!=targets.digest())throw std::invalid_argument("vanity identity does not match plan");
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return std::tie(a.offset,a.target)<std::tie(b.offset,b.target);});
    std::vector<XPointMatch> matches;matches.reserve(candidates.size());
    uint64_t previous=0;uint32_t target=0;uint8_t encoding=0;
    UncompressedPublicKey public_key{};std::string address;
    for(const auto& candidate:candidates) {
        if(candidate.offset>=batch.step_count() || candidate.target>=targets.values().size() || candidate.reserved)
            throw std::runtime_error("invalid GPU vanity candidate record");
        if(!matches.empty() && previous==candidate.offset && target==candidate.target)
            throw std::runtime_error("duplicate GPU vanity relation");
        const auto scalar=batch.scalar_at(candidate.offset);const auto& expected=targets.values()[candidate.target];
        const bool fresh=matches.empty() || previous!=candidate.offset;
        if(fresh)public_key=verifier.derive(scalar);
        if(fresh || encoding!=expected[0])address=bitcoin_address(public_key,expected[0]);
        if(!vanity_matches(address,expected))throw std::runtime_error("GPU vanity candidate failed CPU verification");
        matches.push_back({scalar,candidate.target});previous=candidate.offset;target=candidate.target;encoding=expected[0];
    }
    return matches;
}
} // namespace keyhunt::core
