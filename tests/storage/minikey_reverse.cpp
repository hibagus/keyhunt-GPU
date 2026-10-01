#include "checkpoint_fixture.h"
#include "checkpoint_data.h"
#include <iostream>
using namespace fixture;using namespace keyhunt::storage::detail;
int main(){try{
    core::XPointVerifier verifier;
    for(const bool resume_reverse:{false,true})for(unsigned seconds:{0U,180U})for(const std::string key:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
        // Isolate scenarios so journal.check() does not repeatedly audit all
        // earlier jobs under sanitizers. Every case still tests real receipts.
        Temporary temporary;Journal journal(temporary.path.string());
        const auto project=journal.create_project("Minikey ordinal recovery");
        const auto begin=core::minikey_ordinal(key);const auto length=key.size();
        std::vector<core::MinikeyTarget> values;std::vector<UInt256> ordinals;
        for(unsigned i=0;ordinals.size()<4;++i){const auto at=begin.add(UInt256(i));
            const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
            ordinals.push_back(at);const auto pub=verifier.derive(*scalar);
            for(uint8_t tag:{1,2})values.push_back(core::minikey_target(length,core::hash160_target(pub,tag)));}
        const core::MinikeyTargets targets(values);const auto end=ordinals.back().add(UInt256(1));const auto width=end.subtract(begin);
        const auto scope=CheckpointRun::create_minikeys(journal,project,{begin,end},width,targets);
        const auto grant=journal.claim(scope,"worker","claim-"+std::to_string(length)).at(0);
        CheckpointOptions options;options.xpoint_steps=width.to_uint64();options.candidate_capacity=2;options.checkpoint_seconds=0;options.minikey_reverse=true;
        auto runner=[&](const scheduler::KernelBatch& batch){
            backend::MinikeysResult result{batch,{}};result.device_steps=batch.step_count();
            for(uint64_t i=0;i<batch.step_count();++i){const auto at=batch.ordinal_at(i);
                const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
                const auto pub=verifier.derive(*scalar);
                for(uint8_t tag:{1,2}){const auto target=core::minikey_target(length,core::hash160_target(pub,tag));
                    const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),target);
                    if(found!=targets.values().end()&&*found==target)result.matches.push_back({at,uint32_t(found-targets.values().begin())});}}
            result.candidate_count=result.matches.size();result.overflow=result.candidate_count>options.candidate_capacity;
            if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;return result;
        };
        bool interrupted=false,cleaned=false;
        rejects([&]{CheckpointRun::minikeys(journal,grant,targets,verifier,runner,options,
            [&](const auto&,size_t,double){interrupted=true;throw std::runtime_error("lost committed acknowledgment");},[&]{cleaned=true;});});
        require(interrupted&&cleaned&&journal.results(scope).size()==2,"ordinal result acknowledgment lost");
        const auto retained=journal.block(scope,UInt256()).covered;
        require(retained.size()==1&&retained[0].end()==end&&retained[0].size()==UInt256(1),"overflow credited rejected prefix");
        options.minikey_reverse=resume_reverse;options.work_unit_seconds=seconds;
        auto cursor=resume_reverse?end.subtract(UInt256(1)):begin;
        const auto complete=CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
            require(batch.ordinal_reverse()==resume_reverse,"resume direction not in batch");
            require((resume_reverse?batch.interval().end():batch.interval().begin())==cursor,"resume skipped/repeated ordinals");
            auto result=runner(batch);if(!result.overflow)cursor=resume_reverse?batch.interval().begin():batch.interval().end();return result;
        },options);
        require(cursor==(resume_reverse?begin:end.subtract(UInt256(1))),"resume did not finish complement");
        require(complete.complete&&complete.resumed_scalars==UInt256(1)&&complete.computed_scalars==width.subtract(UInt256(1)),"ordinal complement differs");
        const auto rows=journal.results(scope);require(rows.size()==8,"encoding relation lost");
        for(const auto& row:rows)require(row.target_bytes.size()==22&&std::find(ordinals.begin(),ordinals.end(),row.scalar)!=ordinals.end(),"journal stored derived scalar instead of ordinal");
        require(CheckpointRun::minikeys(journal,grant,targets,verifier,[](const auto&)->backend::MinikeysResult{throw std::runtime_error("finished grant ran");},options).batches==0,"finished retry executed");
        auto changed=values[0];changed[0]=length==22?30:22;
        rejects([&]{CheckpointRun::minikeys(journal,grant,core::MinikeyTargets({changed}),verifier,runner,options);});
        const auto space=core::minikey_space_end(length);rejects([&]{CheckpointRun::create_minikeys(journal,project,{space,space.add(UInt256(1))},UInt256(1),targets);});
        const auto input=binding(targets);auto bytes=input.targets;bytes[0]=26;
        rejects([&]{decode_binding(journal.manifest(scope),input.configuration,bytes);});
        auto config=input.configuration;config[17]=1;rejects([&]{decode_binding(journal.manifest(scope),config,input.targets);});
        options.minikey_reverse=false;
        const auto bad_scope=CheckpointRun::create_minikeys(journal,project,{begin,begin.add(UInt256(2))},UInt256(2),targets);
        const auto bad=journal.claim(bad_scope,"worker","bad-"+std::to_string(length)).at(0);options.xpoint_steps=1;options.candidate_capacity=64;
        for(unsigned fault=0;fault<5;++fault){
            rejects([&]{CheckpointRun::minikeys(journal,bad,targets,verifier,[&](const auto& batch){auto result=runner(batch);
                if(fault==0)result.matches[1]=result.matches[0];
                if(fault==1)result.matches[0].scalar=begin.add(UInt256(1));
                if(fault==2)--result.verified_steps;
                if(fault==3){result.candidate_count=2*batch.step_count()+1;result.overflow=true;result.verified_steps=0;result.matches.clear();}
                if(fault==4){
                    // Identical coverage bounds with the opposite lane mapping
                    // must still fail the completion identity check.
                    auto id=batch.work().identity();id.algorithm=scheduler::WorkAlgorithm::ReverseMinikeysV1;
                    scheduler::BlockGrid grid(batch.work().block_interval(),batch.work().block_interval().size());
                    const auto work=*scheduler::WorkUnit::plan(grid,UInt256(),batch.interval().end(),batch.step_count(),id);
                    result.batch=*scheduler::KernelBatch::plan(work,batch.interval().end(),batch.step_count());
                }
                return result;},options);});
            require(journal.results(bad_scope).empty()&&journal.block(bad_scope,UInt256()).covered.empty(),"invalid ordinal receipt committed");
        }
        options.candidate_capacity=1;rejects([&]{CheckpointRun::minikeys(journal,bad,targets,verifier,runner,options);});options.candidate_capacity=64;
        CheckpointRequest request=CheckpointRequest::Run;bool paused=false;unsigned batches=0;CheckpointControl controls;
        controls.poll=[&]{return request;};controls.notify=[&](CheckpointActivity activity){if(activity==CheckpointActivity::Paused){paused=true;require(journal.results(bad_scope).size()==2,"pause lost results");}};
        controls.wait=[&]{request=CheckpointRequest::Run;};
        const auto resumed=CheckpointRun::minikeys(journal,bad,targets,verifier,[&](const auto& batch){auto result=runner(batch);if(++batches==1)request=CheckpointRequest::Pause;return result;},options,{},{},controls);
        require(paused&&resumed.complete&&journal.results(bad_scope).size()==2,"ordinal cursor did not resume");journal.check();
    }
    Temporary temporary;Journal journal(temporary.path.string());
    // Even an explicit default is rejected outside the ordinal domain.
    const auto scalar_project=journal.create_project("wrong ordinal mode");
    const auto xt=x_targets(verifier,{1});
    const auto scope=CheckpointRun::create_xpoint(journal,scalar_project,{UInt256(1),UInt256(3)},UInt256(2),xt);
    const auto grant=journal.claim(scope,"worker","wrong-mode").at(0);
    CheckpointOptions wrong;wrong.minikey_reverse=false;bool executed=false;
    rejects([&]{CheckpointRun::xpoint(journal,grant,xt,verifier,[&](const auto& batch){executed=true;return execute(batch,xt,verifier,1024);},wrong);});
    require(!executed&&journal.block(scope,UInt256()).covered.empty(),"ordinal option reached scalar runner");
    std::cout<<"Reverse minikey overflow, lost acknowledgment, both-direction adaptive restart and false-receipt rejection passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
