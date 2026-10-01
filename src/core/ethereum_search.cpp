#include "keyhunt/core/ethereum_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "sha3/sha3.h"
#include <algorithm>
#include <fstream>

namespace keyhunt::core {
namespace {
constexpr size_t max_targets = 1048576;
void keccak(const uint8_t* input, size_t length, uint8_t* digest) {
    // Keep CPU verification independent of the portable GPU permutation.
    SHA3_256_CTX context;
    KECCAK_256_Init(&context);
    KECCAK_256_Update(&context,input,length);
    KECCAK_256_Final(digest,&context);
}
unsigned digit(char c) {
    if(c>='0' && c<='9')return unsigned(c-'0');
    if(c>='a' && c<='f')return unsigned(c-'a'+10);
    if(c>='A' && c<='F')return unsigned(c-'A'+10);
    throw std::invalid_argument("Ethereum address must contain 40 hexadecimal digits");
}
}
EthereumTarget ethereum_target(const UncompressedPublicKey& public_key) {
    if(public_key[0]!=4)throw std::invalid_argument("Ethereum verifier needs an uncompressed SEC1 public key");
    alignas(uint64_t) uint8_t digest[32];
    keccak(public_key.data()+1,64,digest);
    EthereumTarget result{};
    std::copy_n(digest+12,20,result.begin());
    return result;
}
EthereumTarget parse_ethereum_address(const std::string& input) {
    const auto text=input.compare(0,2,"0x")==0?input.substr(2):input;
    if(text.size()!=40)throw std::invalid_argument("Ethereum address must contain 40 hexadecimal digits with optional 0x prefix");
    EthereumTarget result{};
    std::string lower(40,'0');
    bool has_lower=false,has_upper=false;
    for(size_t i=0;i<40;++i) {
        const unsigned value=digit(text[i]);
        lower[i]="0123456789abcdef"[value];
        has_lower|=text[i]>='a' && text[i]<='f';
        has_upper|=text[i]>='A' && text[i]<='F';
        result[i/2]|=uint8_t(value<<(i%2?0:4));
    }
    // ERC-55 hashes lowercase ASCII hex, excluding 0x. Uniform-case input is
    // intentionally accepted as raw hex; mixed case asserts a checksum.
    if(has_lower && has_upper) {
        alignas(uint64_t) uint8_t digest[32];
        keccak(reinterpret_cast<const uint8_t*>(lower.data()),lower.size(),digest);
        for(size_t i=0;i<40;++i)if(lower[i]>='a') {
            const unsigned nibble=(digest[i/2]>>(i%2?0:4))&15;
            const char expected=nibble>=8?char(lower[i]-'a'+'A'):lower[i];
            if(text[i]!=expected)throw std::invalid_argument("Ethereum ERC-55 checksum mismatch");
        }
    }
    return result;
}
EthereumTargets::EthereumTargets(std::vector<EthereumTarget> values):values_(std::move(values)) {
    if(values_.empty() || values_.size()>max_targets)
        throw std::invalid_argument("Ethereum target count must be in [1, 1048576]");
    std::sort(values_.begin(),values_.end());
    values_.erase(std::unique(values_.begin(),values_.end()),values_.end());
    std::vector<uint8_t> encoded{'e','t','h','e','r','e','u','m','-','v','1',0};
    encoded.reserve(encoded.size()+20*values_.size());
    for(const auto& target:values_)encoded.insert(encoded.end(),target.begin(),target.end());
    sha256(encoded.data(),encoded.size(),digest_.data());
}
EthereumTargets EthereumTargets::load(const std::string& path) {
    std::ifstream stream(path);
    if(!stream)throw std::runtime_error("cannot open Ethereum targets: "+path);
    std::vector<EthereumTarget> values;
    char line[45]; // 0x + 40 hex digits + optional CR + terminator, bounded before allocation.
    while(stream.getline(line,sizeof(line))) {
        const size_t length=size_t(stream.gcount())-(stream.eof()?0:1);
        std::string text(line,length);
        if(!text.empty() && text.back()=='\r')text.pop_back();
        if(text.empty())continue;
        if(values.size()==max_targets)throw std::invalid_argument("too many Ethereum targets");
        values.push_back(parse_ethereum_address(text));
    }
    if(stream.bad() || !stream.eof())throw std::runtime_error("failed to read Ethereum target file or line too long");
    return EthereumTargets(std::move(values));
}
std::vector<XPointMatch> verify_ethereum(const scheduler::KernelBatch& batch,
    const EthereumTargets& targets,std::vector<XPointCandidate> candidates,const XPointVerifier& verifier) {
    if(scheduler::scalar_family(batch.work().identity().algorithm)!=scheduler::WorkAlgorithm::DirectEthereumV1 ||
       batch.work().identity().target_digest!=targets.digest())
        throw std::invalid_argument("Ethereum target identity does not match plan");
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
    std::vector<XPointMatch> matches;
    matches.reserve(candidates.size());
    uint64_t previous=0;
    for(const auto& candidate:candidates) {
        if(candidate.offset>=batch.step_count() || candidate.target>=targets.values().size() || candidate.reserved)
            throw std::runtime_error("invalid GPU Ethereum candidate record");
        if(!matches.empty() && previous==candidate.offset)
            throw std::runtime_error("duplicate GPU Ethereum scalar");
        const auto scalar=batch.scalar_at(candidate.offset);
        if(ethereum_target(verifier.derive(scalar))!=targets.values()[candidate.target])
            throw std::runtime_error("GPU Ethereum candidate failed CPU verification");
        matches.push_back({batch.coordinate_at(candidate.offset),candidate.target});
        previous=candidate.offset;
    }
    return matches;
}
} // namespace keyhunt::core
