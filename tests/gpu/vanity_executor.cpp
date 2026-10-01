#include "keyhunt/backend/gpu_vanity.h"
#include "keyhunt/scheduler/xpoint_batch_size.h"
#include <iostream>
#include <limits>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
scheduler::KernelBatch plan(UInt256 begin,uint64_t count,const core::VanityTargets& targets,
                            scheduler::WorkAlgorithm algorithm=scheduler::WorkAlgorithm::DirectVanityV1){
    scheduler::ExecutionIdentity identity;identity.algorithm=algorithm;
    identity.target_digest=targets.digest();identity.assignment_id[0]=1;
    identity.assignment_generation=identity.executor_generation=1;
    scheduler::BlockGrid grid(core::ScalarInterval(begin,begin.add(UInt256(count))),UInt256(count));
    auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),begin,count,identity);
    return *scheduler::KernelBatch::plan(work,begin,count);
}
int main(){try{
    core::XPointVerifier verifier;
    const auto begin=UInt256::from_hex("100000000fffffffffffffffe");
    std::vector<core::VanityTarget> values;
    for(unsigned i=0;i<1025;++i){const auto pub=verifier.derive(begin.add(UInt256(i)));
        values.push_back(core::vanity_target(core::bitcoin_address(pub,1),1));values.push_back(core::vanity_target(core::bitcoin_address(pub,2),2));}
    core::VanityTargets targets(values);
    backend::VanityOptions options;options.max_steps=1025;options.candidate_capacity=2050;
    rejects([&]{backend::GpuVanityExecutor e(-1,targets,verifier,options);});
    for(auto capacity:{0U,1U,1048577U})rejects([&]{auto bad=options;bad.candidate_capacity=capacity;backend::GpuVanityExecutor e(0,targets,verifier,bad);});
    for(auto steps:{0U,1048577U})rejects([&]{auto bad=options;bad.max_steps=steps;backend::GpuVanityExecutor e(0,targets,verifier,bad);});
    rejects([&]{auto bad=options;bad.memory_reserve_bytes=std::numeric_limits<uint64_t>::max();backend::GpuVanityExecutor e(0,targets,verifier,bad);});
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped}){
        options.kernel=kernel;
        backend::GpuVanityExecutor e(0,targets,verifier,options),other(0,targets,verifier,options);
        auto first=plan(begin,257,targets);auto ticket=e.submit(first);
        rejects([&]{e.submit(first);});rejects([&]{other.poll(ticket);});
        auto second=other.submit(first);other.drain();require(other.take(second).matches.size()==514,"independent owner failed");
        e.drain();const auto kept=e.take(ticket);
        require(kept.matches.size()==514&&kept.verified_steps==257&&kept.batch.work().identity()==first.work().identity(),"receipt mismatch");
        rejects([&]{e.take(ticket);});
        for(unsigned count:{1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025}){
            auto next=e.submit(plan(begin,count,targets));rejects([&]{e.poll(ticket);});e.drain();auto result=e.take(next);
            require(!result.overflow&&result.device_steps==count&&result.verified_steps==count&&result.matches.size()==2*count,"dense tail mismatch");
            for(unsigned i=0;i<count;++i)for(unsigned tag=0;tag<2;++tag)
                require(result.matches[2*i+tag].scalar==begin.add(UInt256(i)),"missing encoding or scalar");
        }
        auto small=options;small.candidate_capacity=targets.max_matches_per_scalar();
        backend::GpuVanityExecutor bounded(0,targets,verifier,small);
        auto t=bounded.submit(first);bounded.drain();auto result=bounded.take(t);
        require(result.overflow&&result.candidate_count==514&&!result.verified_steps&&result.matches.empty(),"overflow credited prefix");
        scheduler::XPointBatchSize sizing(257,targets.max_matches_per_scalar(),targets.max_matches_per_scalar());sizing.overflow(257);
        require(sizing.limit()==1,"dense two-encoding replay does not fit capacity");
        t=bounded.submit(plan(begin,sizing.limit(),targets));bounded.drain();result=bounded.take(t);
        require(!result.overflow&&result.matches.size()==2&&result.verified_steps==1,"one-scalar replay failed");
        rejects([&]{e.submit(plan(begin,1026,targets));});
        rejects([&]{e.submit(plan(begin,1,targets,scheduler::WorkAlgorithm::DirectXPointV1));});
        rejects([&]{e.submit(plan(begin,1,core::VanityTargets({values[0]})));});
        require(kept.matches.back().scalar==begin.add(UInt256(256)),"owned receipt mutated");
        {backend::GpuVanityExecutor pending(0,targets,verifier,options);pending.submit(first);}
    }
    // Every prefix of both public scalar-1 addresses matches simultaneously.
    // A second scalar makes the broad prefix overflow; none of that attempt is credited.
    std::vector<core::VanityTarget> overlapping;
    for(uint8_t tag:{1,2}){
        const auto address=core::bitcoin_address(verifier.derive(UInt256(1)),tag);
        for(size_t length=1;length<=address.size();++length)
            overlapping.push_back(core::vanity_target(address.substr(0,length),tag));
    }
    core::VanityTargets all(overlapping);options.max_steps=2;options.candidate_capacity=all.max_matches_per_scalar();
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped}){
        options.kernel=kernel;backend::GpuVanityExecutor e(0,all,verifier,options);
        auto ticket=e.submit(plan(UInt256(1),2,all));e.drain();auto result=e.take(ticket);
        require(result.overflow&&result.matches.empty()&&!result.verified_steps,"overlap overflow credited prefix");
        ticket=e.submit(plan(UInt256(1),1,all));e.drain();result=e.take(ticket);
        require(!result.overflow&&result.matches.size()==overlapping.size(),"maximum prefix overlap lost relations");
    }
    std::cout<<"vanity direct/stepped ownership, every-index tails and two-encoding overflow replay passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
