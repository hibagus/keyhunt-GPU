#include "keyhunt/core/hash160_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "keyhunt/crypto/hash/ripemd160.h"
#include "base58/libbase58.h"
#include <algorithm>
#include <fstream>
#include <tuple>

namespace keyhunt::core {
namespace {
constexpr size_t max_targets=1048576;
uint8_t digit(char c) {
    if(c>='0'&&c<='9')return uint8_t(c-'0');
    if(c>='a'&&c<='f')return uint8_t(c-'a'+10);
    if(c>='A'&&c<='F')return uint8_t(c-'A'+10);
    throw std::invalid_argument("HASH160 must have 40 hexadecimal digits");
}
Hash160Bytes decode(const std::string& text,Hash160Input input) {
    Hash160Bytes result{};
    if(input==Hash160Input::Hex) {
        if(text.size()!=40)throw std::invalid_argument("HASH160 must have 40 hexadecimal digits");
        for(size_t i=0;i<20;++i)result[i]=uint8_t(16*digit(text[2*i])+digit(text[2*i+1]));
    } else if(input==Hash160Input::BitcoinAddress) {
        alignas(uint64_t) uint8_t bytes[25]{},first[32],second[32];size_t size=sizeof(bytes);
        if(!b58tobin(bytes,&size,text.data(),text.size())||size!=25||bytes[0]!=0)
            throw std::invalid_argument("address must be a Bitcoin mainnet P2PKH Base58Check address");
        sha256(bytes,21,first);sha256(first,32,second);
        if(!std::equal(bytes+21,bytes+25,second))throw std::invalid_argument("Bitcoin address checksum mismatch");
        // Decode/re-encode also excludes redundant leading zeroes. Do not use
        // libbase58's mutable global hash callback for checksum verification.
        char canonical[40];size_t length=sizeof(canonical);
        if(!b58enc(canonical,&length,bytes,sizeof(bytes))||text!=canonical)
            throw std::invalid_argument("noncanonical Bitcoin address");
        std::copy_n(bytes+1,20,result.begin());
    } else throw std::invalid_argument("unsupported HASH160 input format");
    return result;
}
}
Hash160Encoding hash160_encoding(const std::string& name) {
    if(name=="compressed")return Hash160Encoding::Compressed;
    if(name=="uncompressed")return Hash160Encoding::Uncompressed;
    if(name=="both")return Hash160Encoding::Both;
    throw std::invalid_argument("encoding must be compressed, uncompressed or both");
}
const char* hash160_encoding_name(uint8_t tag) {
    if(tag==1)return "compressed";
    if(tag==2)return "uncompressed";
    throw std::invalid_argument("invalid HASH160 encoding tag");
}
Hash160Target hash160_target(const UncompressedPublicKey& public_key,uint8_t tag) {
    hash160_encoding_name(tag);
    if(public_key[0]!=4)throw std::invalid_argument("HASH160 verifier needs an uncompressed SEC1 public key");
    // The retained CPU hash functions accept aligned storage internally. Keep
    // that requirement inside this adapter, independent of target-array layout.
    alignas(uint64_t) uint8_t serialized[65],digest[32],hash[20];
    std::copy(public_key.begin(),public_key.end(),serialized);
    if(tag==1)serialized[0]=uint8_t(2+(public_key[64]&1));
    sha256(serialized,tag==1?33:65,digest);ripemd160(digest,32,hash);
    Hash160Target result{};result[0]=tag;std::copy_n(hash,20,result.begin()+1);return result;
}
Hash160Targets::Hash160Targets(std::vector<Hash160Target> values):values_(std::move(values)) {
    if(values_.empty()||values_.size()>max_targets)
        throw std::invalid_argument("HASH160 target count must be in [1, 1048576], including encoding tags");
    for(const auto& target:values_){hash160_encoding_name(target[0]);encodings_|=target[0];}
    std::sort(values_.begin(),values_.end());values_.erase(std::unique(values_.begin(),values_.end()),values_.end());
    std::vector<uint8_t> encoded{'h','a','s','h','1','6','0','-','v','1',0};
    encoded.reserve(encoded.size()+21*values_.size());
    for(const auto& target:values_)encoded.insert(encoded.end(),target.begin(),target.end());
    sha256(encoded.data(),encoded.size(),digest_.data());
}
Hash160Targets Hash160Targets::load(const std::string& path,Hash160Input input,Hash160Encoding encoding) {
    const auto mask=uint8_t(encoding);
    if(mask<1||mask>3)throw std::invalid_argument("invalid HASH160 encoding selection");
    std::ifstream stream(path);if(!stream)throw std::runtime_error("cannot open HASH160 targets: "+path);
    std::vector<Hash160Target> values;char line[43];
    while(stream.getline(line,sizeof(line))) {
        const size_t length=size_t(stream.gcount())-(stream.eof()?0:1);
        std::string text(line,length);if(!text.empty()&&text.back()=='\r')text.pop_back();
        if(text.empty())continue;
        const auto hash=decode(text,input);
        for(uint8_t tag:{1,2})if(mask&tag){
            if(values.size()==max_targets)throw std::invalid_argument("too many HASH160 targets");
            Hash160Target target{};target[0]=tag;std::copy(hash.begin(),hash.end(),target.begin()+1);values.push_back(target);
        }
    }
    if(stream.bad()||!stream.eof())throw std::runtime_error("failed to read HASH160 target file or line too long");
    return Hash160Targets(std::move(values));
}
std::vector<XPointMatch> verify_hash160(const scheduler::KernelBatch& batch,
    const Hash160Targets& targets,std::vector<XPointCandidate> candidates,const XPointVerifier& verifier) {
    if(batch.work().identity().algorithm!=scheduler::WorkAlgorithm::DirectHash160V1 ||
       batch.work().identity().target_digest!=targets.digest())throw std::invalid_argument("HASH160 target digest does not match plan");
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return std::tie(a.offset,a.target)<std::tie(b.offset,b.target);});
    std::vector<XPointMatch> matches;matches.reserve(candidates.size());
    uint64_t previous=0;uint32_t target=0;UncompressedPublicKey public_key{};
    for(const auto& candidate:candidates) {
        if(candidate.offset>=batch.step_count()||candidate.target>=targets.values().size()||candidate.reserved)
            throw std::runtime_error("invalid GPU HASH160 candidate record");
        if(!matches.empty()&&previous==candidate.offset&&target==candidate.target)
            throw std::runtime_error("duplicate GPU HASH160 candidate");
        const auto scalar=batch.scalar_at(candidate.offset);
        if(matches.empty()||previous!=candidate.offset)public_key=verifier.derive(scalar);
        const auto& expected=targets.values()[candidate.target];
        if(hash160_target(public_key,expected[0])!=expected)throw std::runtime_error("GPU HASH160 candidate failed CPU verification");
        matches.push_back({scalar,candidate.target});previous=candidate.offset;target=candidate.target;
    }
    return matches;
}
} // namespace keyhunt::core
