// A test executor for the C05 plan + C04 verifier contract, not a search backend.
#include "keyhunt/scheduler/work_unit.h"
#include "keyhunt/core/result_verifier.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace keyhunt::core;
using namespace keyhunt::scheduler;

namespace {
template<size_t N> std::array<uint8_t,N> bytes(const std::string& text) {
    if (text.size()!=N*2) throw std::invalid_argument("bad target size");
    std::array<uint8_t,N> result{};
    auto digit=[](char c) {
        if (c>='0' && c<='9') return c-'0';
        if (c>='a' && c<='f') return c-'a'+10;
        throw std::invalid_argument("bad target hex");
    };
    for (size_t i=0;i<N;++i) result[i]=digit(text[2*i])*16+digit(text[2*i+1]);
    return result;
}
struct Target {
    bool xpoint;
    XPointBytes x{};
    UncompressedPublicKey pub{};
};
std::string execute(CpuResultVerifier& verifier,const std::vector<std::string>& words) {
    if (words.size()<5) throw std::invalid_argument("missing plan");
    const ScalarInterval root(UInt256::from_hex(words[0]),UInt256::from_hex(words[1]));
    if (root.size()>UInt256(256)) throw std::invalid_argument("test executor only supports tiny jobs");
    const BlockGrid grid(root,UInt256::from_hex(words[2]));
    const auto work_limit=UInt256::from_hex(words[3]).to_uint64();
    const auto batch_limit=UInt256::from_hex(words[4]).to_uint64();
    ExecutionIdentity identity;
    identity.assignment_id.back()=1;
    identity.assignment_generation=1;
    identity.executor_generation=1;
    std::vector<Target> targets;
    for (size_t i=5;i<words.size();++i) {
        Target target{};
        target.xpoint=words[i].substr(0,2)=="x:";
        if (target.xpoint) target.x=bytes<32>(words[i].substr(2));
        else if (words[i].substr(0,2)=="p:") target.pub=bytes<65>(words[i].substr(2));
        else throw std::invalid_argument("bad target kind");
        targets.push_back(target);
    }
    UInt256 visited;
    std::string hits;
    for (UInt256 id; id<grid.count(); id=id.add(UInt256(1))) {
        auto cursor=grid.block(id).begin();
        while (auto work=WorkUnit::plan(grid,id,cursor,work_limit,identity)) {
            auto batch_cursor=cursor;
            while (auto batch=KernelBatch::plan(*work,batch_cursor,batch_limit)) {
                for (uint64_t i=0;i<batch->step_count();++i) {
                    const auto scalar=batch->scalar_at(i);
                    if (!root.contains(scalar)) throw std::runtime_error("out-of-range candidate");
                    visited=visited.add(UInt256(1));
                    for (size_t t=0;t<targets.size();++t) {
                        const auto& target=targets[t];
                        const bool match=target.xpoint ? verifier.matches_xpoint(scalar.bytes(),target.x)
                            : verifier.matches_public_key(scalar.bytes(),target.pub);
                        if (match) hits+=" "+scalar.hex()+":"+std::to_string(t);
                    }
                }
                // Test only: simulated acceptance, without any durable coverage.
                batch_cursor=batch->interval().end();
            }
            cursor=work->interval().end();
        }
    }
    return visited.hex()+hits;
}
}
int main() {
    auto curve=std::make_unique<Secp256K1>();
    curve->Init();
    CpuResultVerifier verifier(*curve);
    for (std::string line;std::getline(std::cin,line);) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word;input>>word;) words.push_back(word);
        try { const auto result=execute(verifier,words); std::cout<<result<<'\n'; }
        catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    }
}
