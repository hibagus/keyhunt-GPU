#pragma once
#include "keyhunt/storage/checkpoint.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>
namespace fixture {
using namespace keyhunt;
using namespace storage;
inline void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
struct Temporary {
    std::filesystem::path path;
    Temporary(){char p[]="/tmp/keyhunt-c13-XXXXXX";require(mkdtemp(p),"mkdtemp");path=p;}
    ~Temporary(){std::filesystem::remove_all(path);}
};
inline core::XPointTargets x_targets(const core::XPointVerifier& verifier,std::initializer_list<uint64_t> scalars){
    std::vector<core::XPointBytes> out;
    for(auto n:scalars){const auto pub=verifier.derive(UInt256(n));core::XPointBytes x;std::copy_n(pub.begin()+1,32,x.begin());out.push_back(x);}
    return core::XPointTargets(std::move(out));
}
inline core::BsgsPublicKeyTargets b_targets(const core::XPointVerifier& verifier,std::initializer_list<uint64_t> scalars){
    std::vector<core::UncompressedPublicKey> out;for(auto n:scalars)out.push_back(verifier.derive(UInt256(n)));
    return core::BsgsPublicKeyTargets(std::move(out));
}
// A tiny CPU executor computes real public points; the checkpoint owner still
// performs its independent acceptance checks and all normal journal transitions.
inline backend::XPointResult execute(const scheduler::KernelBatch& batch,const core::XPointTargets& targets,
    const core::XPointVerifier& verifier,uint32_t capacity){
    backend::XPointResult result{batch,{}};result.device_steps=batch.step_count();
    for(uint64_t i=0;i<batch.step_count();++i){
        const auto scalar=batch.scalar_at(i);const auto pub=verifier.derive(scalar);core::XPointBytes x;
        std::copy_n(pub.begin()+1,32,x.begin());const auto it=std::lower_bound(targets.values().begin(),targets.values().end(),x);
        if(it!=targets.values().end() && *it==x)result.matches.push_back({scalar,uint32_t(it-targets.values().begin())});
    }
    result.candidate_count=result.matches.size();result.overflow=result.candidate_count>capacity;
    if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
    return result;
}
inline backend::BsgsSearchResult execute(const core::BsgsBatch& batch,const core::BsgsPublicKeyTargets& targets,
    const core::XPointVerifier& verifier,uint32_t capacity){
    backend::BsgsSearchResult result{batch,{}};result.device_steps=batch.steps();result.group_size=1;
    for(auto scalar=batch.interval().begin();scalar<batch.interval().end();scalar=scalar.add(UInt256(1))){
        const auto pub=verifier.derive(scalar);
        for(uint32_t t=batch.first_target();t<batch.first_target()+batch.target_count();++t)
            if(pub==targets.values()[t])result.matches.push_back({scalar,t});
    }
    result.candidate_count=result.matches.size();result.overflow=result.candidate_count>capacity;
    if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
    return result;
}
}
