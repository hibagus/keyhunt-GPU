#include "checkpoint_data.h"
#include <algorithm>
#include <stdexcept>

namespace keyhunt::storage::detail {
namespace {
void number(Bytes& b,uint64_t n){for(int i=7;i>=0;--i)b.push_back(uint8_t(n>>(8*i)));}
void wide(Bytes& b,const UInt256& n){const auto a=n.bytes();b.insert(b.end(),a.begin(),a.end());}
Digest fixed(const Bytes& b){Digest out{};std::copy_n(b.begin(),32,out.begin());return out;}
Bytes configuration(Mode mode,uint64_t m,const Digest& table) {
    Bytes out{'k','h','s','e','a','r','c','h',1,uint8_t(mode)};
    number(out,m);out.insert(out.end(),table.begin(),table.end());return out;
}
Binding make(Mode mode,const Bytes& targets,const Digest& digest,uint64_t m,const Digest& table) {
    const auto config=configuration(mode,m,table);
    return {mode,config,targets,digest,fixed(detail::digest(config)),table,m};
}
struct Reader {
    const Bytes& bytes;size_t at=0;
    Bytes take(size_t size){if(size>bytes.size()-at)throw std::runtime_error("short checkpoint");Bytes b(bytes.begin()+at,bytes.begin()+at+size);at+=size;return b;}
    uint64_t number(){uint64_t n=0;for(auto c:take(8))n=(n<<8)|c;return n;}
    UInt256 wide(){auto b=take(32);UInt256::Bytes a{};std::copy(b.begin(),b.end(),a.begin());return UInt256::from_bytes(a);}
};
}
Binding binding(const core::XPointTargets& targets) {
    Bytes bytes;for(const auto& t:targets.values())bytes.insert(bytes.end(),t.begin(),t.end());
    return make(Mode::XPoint,bytes,targets.digest(),0,{});
}
Binding binding(const core::Hash160Targets& targets) {
    Bytes bytes;for(const auto& t:targets.values())bytes.insert(bytes.end(),t.begin(),t.end());
    return make(Mode::Hash160,bytes,targets.digest(),0,{});
}
Binding binding(const core::EthereumTargets& targets) {
    Bytes bytes;for(const auto& t:targets.values())bytes.insert(bytes.end(),t.begin(),t.end());
    return make(Mode::Ethereum,bytes,targets.digest(),0,{});
}
Binding binding(const core::BsgsPublicKeyTargets& targets,const bsgs::Table& table) {
    Bytes bytes;for(const auto& t:targets.values())bytes.insert(bytes.end(),t.begin(),t.end());
    return make(Mode::Bsgs,bytes,targets.digest(),table.memory().m,table.checksum());
}
Binding decode_binding(const Manifest& manifest,const Bytes& config,const Bytes& bytes) {
    if(config.size()!=50 || bytes.empty())throw std::runtime_error("invalid search binding length");
    Reader read{config};const auto tag=read.take(10);
    if(tag!=Bytes({'k','h','s','e','a','r','c','h',1,uint8_t(manifest.mode)}))
        throw std::runtime_error("unsupported search semantics");
    const auto m=read.number();const auto checksum=fixed(read.take(32));
    Binding result;
    if(manifest.mode==Mode::XPoint){
        if(m || checksum!=Digest{} || bytes.size()%32 || bytes.size()/32>1048576)
            throw std::runtime_error("invalid xpoint binding");
        std::vector<core::XPointBytes> targets(bytes.size()/32);
        for(size_t i=0;i<targets.size();++i)std::copy_n(bytes.begin()+32*i,32,targets[i].begin());
        result=binding(core::XPointTargets(std::move(targets)));
    }else if(manifest.mode==Mode::Hash160){
        if(m || checksum!=Digest{} || bytes.size()%21 || bytes.size()/21>1048576)
            throw std::runtime_error("invalid HASH160 binding");
        std::vector<core::Hash160Target> targets(bytes.size()/21);
        for(size_t i=0;i<targets.size();++i)std::copy_n(bytes.begin()+21*i,21,targets[i].begin());
        result=binding(core::Hash160Targets(std::move(targets)));
    }else if(manifest.mode==Mode::Ethereum){
        if(m || checksum!=Digest{} || bytes.size()%20 || bytes.size()/20>1048576)
            throw std::runtime_error("invalid Ethereum binding");
        std::vector<core::EthereumTarget> targets(bytes.size()/20);
        for(size_t i=0;i<targets.size();++i)std::copy_n(bytes.begin()+20*i,20,targets[i].begin());
        result=binding(core::EthereumTargets(std::move(targets)));
    }else if(manifest.mode==Mode::Bsgs){
        if(!m || bytes.size()%65 || bytes.size()/65>65536)throw std::runtime_error("invalid BSGS binding");
        std::vector<core::UncompressedPublicKey> targets(bytes.size()/65);
        for(size_t i=0;i<targets.size();++i)std::copy_n(bytes.begin()+65*i,65,targets[i].begin());
        core::BsgsPublicKeyTargets canonical(std::move(targets));Bytes encoded;
        for(const auto& t:canonical.values())encoded.insert(encoded.end(),t.begin(),t.end());
        result=make(Mode::Bsgs,encoded,canonical.digest(),m,checksum);
    }
    else throw std::runtime_error("unsupported checkpoint mode");
    if(result.targets!=bytes || result.configuration!=config ||
       result.target_digest!=manifest.targets || result.algorithm_digest!=manifest.algorithm)
        throw std::runtime_error("canonical search inputs do not match job identity");
    return result;
}
void Binding::verify(const core::XPointVerifier& verifier,const UInt256& scalar,uint32_t target)const{
    if(target>=count())throw std::runtime_error("checkpoint target out of range");
    const auto pub=verifier.derive(scalar);const size_t width=target_width(mode);
    const auto expected=targets.begin()+width*target;
    if(mode==Mode::Hash160){
        const auto hash=core::hash160_target(pub,*expected);
        if(std::equal(hash.begin(),hash.end(),expected))return;
    }else if(mode==Mode::Ethereum){
        const auto address=core::ethereum_target(pub);
        if(std::equal(address.begin(),address.end(),expected))return;
    }else{
        const auto begin=pub.begin()+(mode==Mode::XPoint?1:0);
        if(std::equal(begin,begin+width,expected))return;
    }
    throw std::runtime_error("checkpoint match failed CPU verification");
}
Bytes encode_checkpoint(const CheckpointData& data) {
    if(data.epoch.size()!=16 || !data.generation || data.generation>INT64_MAX ||
       !data.executor || data.executor>INT64_MAX || data.coverage.size()>1024 || data.matches.size()>1048576 ||
       (data.coverage.empty() && data.matches.empty()))throw std::invalid_argument("invalid checkpoint bounds");
    Bytes b{'k','h','c','p',1};wide(b,data.block);number(b,data.generation);number(b,data.executor);
    b.insert(b.end(),data.epoch.begin(),data.epoch.end());number(b,data.coverage.size());
    for(const auto& v:data.coverage){wide(b,v.begin());wide(b,v.end());}
    number(b,data.matches.size());for(const auto& m:data.matches){wide(b,m.scalar);number(b,m.target);}
    return b;
}
CheckpointData decode_checkpoint(const Bytes& bytes) {
    Reader r{bytes};if(r.take(5)!=Bytes({'k','h','c','p',1}))throw std::runtime_error("unsupported checkpoint encoding");
    CheckpointData out;out.block=r.wide();out.generation=r.number();out.executor=r.number();out.epoch=r.take(16);
    const auto count=r.number();if(count>1024)throw std::runtime_error("checkpoint interval bound exceeded");
    for(uint64_t i=0;i<count;++i){const auto lo=r.wide(),hi=r.wide();out.coverage.emplace_back(lo,hi);}
    const auto matches=r.number();if(matches>1048576)throw std::runtime_error("checkpoint match bound exceeded");
    for(uint64_t i=0;i<matches;++i){const auto scalar=r.wide();const auto target=r.number();if(target>UINT32_MAX)throw std::runtime_error("invalid target index");out.matches.push_back({scalar,uint32_t(target)});}
    if(r.at!=bytes.size() || encode_checkpoint(out)!=bytes)throw std::runtime_error("noncanonical checkpoint encoding");
    return out;
}
std::vector<ScalarInterval> merged(std::vector<ScalarInterval> intervals) {
    std::sort(intervals.begin(),intervals.end(),[](const auto& a,const auto& b){return a.begin()<b.begin();});
    std::vector<ScalarInterval> out;
    for(const auto& v:intervals){
        if(out.empty() || out.back().end()<v.begin())out.push_back(v);
        else out.back()=ScalarInterval(out.back().begin(),std::max(out.back().end(),v.end()));
    }
    return out;
}
} // namespace keyhunt::storage::detail
