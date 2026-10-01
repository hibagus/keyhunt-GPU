#include "keyhunt/backend/gpu_minikeys.h"
#include "keyhunt/scheduler/xpoint_batch_size.h"
#include <algorithm>
#include <iostream>
#include <limits>
using namespace keyhunt;using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
scheduler::KernelBatch plan(UInt256 begin,uint64_t count,const core::MinikeyTargets& targets,
    scheduler::WorkAlgorithm algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1){
    scheduler::ExecutionIdentity id;id.algorithm=algorithm;id.target_digest=targets.digest();
    id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    scheduler::BlockGrid grid({begin,begin.add(UInt256(count))},UInt256(count));
    auto work=*scheduler::WorkUnit::plan(grid,UInt256(),begin,count,id);return *scheduler::KernelBatch::plan(work,begin,count);
}
int main(){try{
    core::XPointVerifier verifier;
    for(const std::string text:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
        const auto begin=core::minikey_ordinal(text);std::vector<core::MinikeyTarget> values;std::vector<UInt256> valid;
        for(unsigned i=0;i<4097;++i){const auto ordinal=begin.add(UInt256(i));const auto scalar=core::minikey_scalar(core::minikey_text(ordinal,text.size()));
            if(!scalar)continue;valid.push_back(ordinal);const auto pub=verifier.derive(*scalar);
            for(uint8_t tag:{1,2})values.push_back(core::minikey_target(text.size(),core::hash160_target(pub,tag)));}
        const core::MinikeyTargets targets(values);backend::MinikeysOptions options;options.max_steps=4097;options.candidate_capacity=128;
        rejects([&]{backend::GpuMinikeysExecutor e(-1,targets,verifier,options);});
        for(auto capacity:{0U,1U,1048577U})rejects([&]{auto bad=options;bad.candidate_capacity=capacity;backend::GpuMinikeysExecutor e(0,targets,verifier,bad);});
        for(auto steps:{0U,1048577U})rejects([&]{auto bad=options;bad.max_steps=steps;backend::GpuMinikeysExecutor e(0,targets,verifier,bad);});
        rejects([&]{auto bad=options;bad.memory_reserve_bytes=UINT64_MAX;backend::GpuMinikeysExecutor e(0,targets,verifier,bad);});
        backend::GpuMinikeysExecutor e(0,targets,verifier,options),other(0,targets,verifier,options);
        const auto first=plan(begin,257,targets);const auto ticket=e.submit(first);
        rejects([&]{e.submit(first);});rejects([&]{other.poll(ticket);});
        auto second=other.submit(first);other.drain();require(other.take(second).verified_steps==257,"independent owner failed");
        e.drain();const auto kept=e.take(ticket);rejects([&]{e.take(ticket);});
        for(unsigned count:{1,2,3,4,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025,4097}){
            auto next=e.submit(plan(begin,count,targets));rejects([&]{e.poll(ticket);});e.drain();const auto result=e.take(next);
            const auto expected=std::count_if(valid.begin(),valid.end(),[&](const auto& at){return at<begin.add(UInt256(count));});
            require(!result.overflow&&result.verified_steps==count&&result.matches.size()==2*size_t(expected),"ordinal tail or admitted relation lost");
        }
        auto small=options;small.candidate_capacity=2;backend::GpuMinikeysExecutor bounded(0,targets,verifier,small);
        auto t=bounded.submit(plan(begin,4097,targets));bounded.drain();auto result=bounded.take(t);
        require(result.overflow&&result.candidate_count==2*valid.size()&&!result.verified_steps&&result.matches.empty(),"overflow credited prefix");
        t=bounded.submit(plan(begin,1,targets));bounded.drain();result=bounded.take(t);
        require(result.verified_steps==1&&result.matches.size()==2,"one-ordinal replay failed");
        rejects([&]{e.submit(plan(begin,4098,targets));});
        rejects([&]{e.submit(plan(begin,1,targets,scheduler::WorkAlgorithm::DirectHash160V1));});
        rejects([&]{e.submit(plan(begin,1,core::MinikeyTargets({values[0]})));});
        const auto end=core::minikey_space_end(text.size());rejects([&]{e.submit(plan(end,1,targets));});
        require(kept.matches.front().scalar==begin,"retained receipt mutated");
        {backend::GpuMinikeysExecutor pending(0,targets,verifier,options);pending.submit(first);}
    }
    std::cout<<"Minikey 22/30 ordinal tails, validity, ownership, bounded output and overflow replay passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
