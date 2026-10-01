#include "checkpoint_fixture.h"
#include <iostream>
#include <set>
using namespace fixture;
int main(){try{
    using core::MinikeyOrder;
    core::XPointVerifier verifier;
    for(const auto resume:{MinikeyOrder::Forward,MinikeyOrder::Reverse,MinikeyOrder::BothEnds})
    for(unsigned seconds:{0U,180U})
    for(const std::string key:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
        Temporary temporary;Journal journal(temporary.path.string());
        const auto project=journal.create_project("Both-ends ordinal recovery");
        const auto begin=core::minikey_ordinal(key);const unsigned length=key.size();
        std::vector<core::MinikeyTarget> values;std::vector<UInt256> ordinals;
        for(auto at=begin;ordinals.size()<4;at=at.add(UInt256(1))){
            const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
            ordinals.push_back(at);const auto point=verifier.derive(*scalar);
            for(uint8_t tag:{1,2})values.push_back(core::minikey_target(length,core::hash160_target(point,tag)));
        }
        const core::MinikeyTargets targets(values);const auto end=ordinals.back().add(UInt256(1)),width=end.subtract(begin);
        const auto scope=CheckpointRun::create_minikeys(journal,project,{begin,end},width,targets);
        const auto grant=journal.claim(scope,"owner","first").at(0);
        CheckpointOptions options;options.minikey_order=MinikeyOrder::BothEnds;
        options.xpoint_steps=width.to_uint64();options.candidate_capacity=2;options.checkpoint_seconds=0;
        auto execute=[&](const scheduler::KernelBatch& batch){
            backend::MinikeysResult result{batch,{}};result.device_steps=batch.step_count();
            for(uint64_t i=0;i<batch.step_count();++i){const auto at=batch.ordinal_at(i);
                const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
                const auto point=verifier.derive(*scalar);
                for(uint8_t tag:{1,2}){const auto target=core::minikey_target(length,core::hash160_target(point,tag));
                    const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),target);
                    if(found!=targets.values().end()&&*found==target)result.matches.push_back({at,uint32_t(found-targets.values().begin())});}
            }
            result.candidate_count=result.matches.size();result.overflow=result.candidate_count>options.candidate_capacity;
            if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
            return result;
        };
        bool high=false,cleaned=false;unsigned overflows=0,notices=0;
        rejects([&]{CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
            require(batch.ordinal_reverse()==high,"overflow changed alternating phase");
            require(batch.ordinal_at(0)==(high?end.subtract(UInt256(1)):begin),"initial endpoint changed");
            auto result=execute(batch);if(result.overflow)++overflows;else high=!high;return result;
        },options,[&](const auto&,size_t,double){
            // Lose the acknowledgment only after both endpoint receipts commit.
            if(++notices==2)throw std::runtime_error("lost second acknowledgment");
        },[&]{cleaned=true;});});
        const auto saved=journal.block(scope,UInt256()).covered;
        require(cleaned&&notices==2&&overflows&&saved.size()==2,"both endpoint receipts were not retained");
        require(saved[0].begin()==begin&&saved[0].size()==UInt256(1)&&saved[1].end()==end&&saved[1].size()==UInt256(1),"overflow credited unverified work");
        require(journal.results(scope).size()==4,"endpoint encoding relations were lost");
        std::set<UInt256> missing;
        for(auto at=begin.add(UInt256(1));at<end.subtract(UInt256(1));at=at.add(UInt256(1)))missing.insert(at);
        options.minikey_order=resume;options.xpoint_steps=17;options.work_unit_seconds=seconds;
        high=resume==MinikeyOrder::Reverse;bool grew=false;
        const auto complete=CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
            require(batch.ordinal_reverse()==high,"restart changed the selected order");
            require(batch.ordinal_at(0)==(high?*missing.rbegin():*missing.begin()),"restart skipped a global endpoint");
            grew|=batch.work().interval().size()>UInt256(17);
            auto result=execute(batch);
            if(!result.overflow){
                for(uint64_t i=0;i<batch.step_count();++i){const auto at=batch.ordinal_at(i);
                    require(at==(high?*missing.rbegin():*missing.begin()),"restart repeated or skipped an ordinal");missing.erase(at);
                }
                if(resume==MinikeyOrder::BothEnds)high=!high;
            }
            return result;
        },options);
        require(complete.complete&&missing.empty()&&complete.resumed_scalars==UInt256(2)&&complete.computed_scalars==width.subtract(UInt256(2)),"restart complement differs");
        require(!seconds||grew,"adaptive reservation did not grow");
        const auto rows=journal.results(scope);require(rows.size()==8,"final encoding relation union differs");
        for(const auto& row:rows)require(std::find(ordinals.begin(),ordinals.end(),row.scalar)!=ordinals.end(),"result is not a canonical ordinal");
        require(CheckpointRun::minikeys(journal,grant,targets,verifier,[](const auto&)->backend::MinikeysResult{throw std::runtime_error("finished job ran");},options).batches==0,"finished retry executed");
        journal.check();
    }
    // Reject a plausible completion with the wrong lane mapping on a high batch,
    // retaining the earlier low receipt and granting no coverage for the fault.
    Temporary temporary;Journal journal(temporary.path.string());const auto project=journal.create_project("False high completion");
    const auto ordinal=core::minikey_ordinal("SzavMBLoXU6kDrqtUVmffv");
    const core::MinikeyTargets targets({core::minikey_target(22,core::hash160_target(verifier.derive(UInt256(1)),1))});
    const auto scope=CheckpointRun::create_minikeys(journal,project,{ordinal,ordinal.add(UInt256(3))},UInt256(3),targets);
    const auto grant=journal.claim(scope,"owner","bad-high").at(0);
    CheckpointOptions options;options.minikey_order=MinikeyOrder::BothEnds;options.xpoint_steps=1;options.checkpoint_seconds=0;
    rejects([&]{CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
        backend::MinikeysResult result{batch,{}};result.device_steps=result.verified_steps=batch.step_count();
        if(batch.ordinal_reverse()){
            auto identity=batch.work().identity();identity.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;
            scheduler::BlockGrid grid(batch.work().block_interval(),batch.work().block_interval().size());
            const auto work=*scheduler::WorkUnit::plan(grid,UInt256(),batch.interval().begin(),1,identity);
            result.batch=*scheduler::KernelBatch::plan(work,batch.interval().begin(),1);
        }
        return result;
    },options);});
    const auto retained=journal.block(scope,UInt256()).covered;
    require(retained.size()==1&&retained[0].begin()==ordinal&&retained[0].size()==UInt256(1),"wrong-direction completion wrote coverage");
    const auto xt=x_targets(verifier,{1});const auto xs=CheckpointRun::create_xpoint(journal,project,{UInt256(1),UInt256(3)},UInt256(2),xt);
    const auto xg=journal.claim(xs,"owner","wrong-mode").at(0);bool executed=false;
    rejects([&]{CheckpointRun::xpoint(journal,xg,xt,verifier,[&](const auto& batch){executed=true;return fixture::execute(batch,xt,verifier,1024);},options);});
    require(!executed,"both-ends ordinal policy reached a scalar executor");journal.check();
    std::cout<<"Both-ends minikey overflow, lost acknowledgments, all restart orders, adaptive work and false high receipts passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
