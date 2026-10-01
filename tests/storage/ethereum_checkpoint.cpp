#include "checkpoint_fixture.h"
#include "checkpoint_data.h"
#include <iostream>
using namespace fixture;
using namespace keyhunt::storage::detail;
backend::EthereumResult execute_hash(const scheduler::KernelBatch& batch,const core::EthereumTargets& targets,
    const core::XPointVerifier& verifier,uint32_t capacity){
    backend::EthereumResult result{batch,{}};result.device_steps=batch.step_count();
    for(uint64_t i=0;i<batch.step_count();++i){
        const auto scalar=batch.scalar_at(i);const auto pub=verifier.derive(scalar);
        {
            const auto hash=core::ethereum_target(pub);
            const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),hash);
            if(found!=targets.values().end()&&*found==hash)result.matches.push_back({scalar,uint32_t(found-targets.values().begin())});
        }
    }
    result.candidate_count=result.matches.size();result.overflow=result.candidate_count>capacity;
    if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
    return result;
}
int main(){try{
    Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
    std::vector<core::EthereumTarget> values;
    for(unsigned scalar:{1,2,5,9})values.push_back(core::ethereum_target(verifier.derive(UInt256(scalar))));
    const core::EthereumTargets targets(values);
    const auto project=journal.create_project("Ethereum recovery");
    const auto scope=CheckpointRun::create_ethereum(journal,project,{UInt256(1),UInt256(10)},UInt256(9),targets);
    const auto grant=journal.claim(scope,"worker","claim").at(0);
    CheckpointOptions options;options.xpoint_steps=4;options.candidate_capacity=1;options.checkpoint_seconds=0;
    const auto runner=[&](const auto& batch){return execute_hash(batch,targets,verifier,options.candidate_capacity);};
    bool interrupted=false,cleaned=false;
    rejects([&]{CheckpointRun::ethereum(journal,grant,targets,verifier,runner,options,
        [&](const auto&,size_t,double){interrupted=true;journal.check();throw std::runtime_error("lost committed acknowledgment");},
        [&]{cleaned=true;rejects([&]{CheckpointRun::ethereum(journal,grant,targets,verifier,runner,options);});});});
    require(interrupted&&cleaned&&journal.results(scope).size()==1,"address commit/cleanup failed");
    const auto retained=journal.block(scope,UInt256()).covered;
    require(retained.size()==1&&retained[0].size()==UInt256(1),"overflow prefix incorrectly covered");
    const auto summary=CheckpointRun::ethereum(journal,grant,targets,verifier,runner,options);
    require(summary.complete&&summary.resumed_scalars==UInt256(1)&&summary.computed_scalars==UInt256(8),"remaining range replay mismatch");
    const auto rows=journal.results(scope);require(rows.size()==4,"address results lost or duplicated");
    for(const auto& row:rows)require(row.target_bytes.size()==20&&
        row.target_bytes==Bytes(targets.values()[row.target].begin(),targets.values()[row.target].end()),"stored target address lost");
    const auto done=CheckpointRun::ethereum(journal,grant,targets,verifier,[](const auto&)->backend::EthereumResult{
        throw std::runtime_error("finished grant executed");},options);
    require(done.batches==0&&done.resumed_scalars==UInt256(9),"finished retry executed");
    const core::EthereumTargets changed({values[0]});
    rejects([&]{CheckpointRun::ethereum(journal,grant,changed,verifier,runner,options);});
    auto invalid=journal.manifest(scope);invalid.mode=Mode(255);
    rejects([&]{journal.create_job(project,invalid);});
    const auto input=binding(targets);
    rejects([&]{decode_binding(invalid,input.configuration,input.targets);});
    auto config=input.configuration;config[17]=1;
    rejects([&]{decode_binding(journal.manifest(scope),config,input.targets);});
    auto bytes=input.targets;bytes[0]^=1;
    rejects([&]{decode_binding(journal.manifest(scope),input.configuration,bytes);});

    // A trusted runner can still return a corrupt receipt. None of these errors
    // may persist a result or claim the accompanying scalar interval.
    const auto bad_scope=CheckpointRun::create_ethereum(journal,project,{UInt256(1),UInt256(4)},UInt256(3),targets);
    const auto bad=journal.claim(bad_scope,"worker","bad").at(0);options.candidate_capacity=64;
    for(unsigned fault=0;fault<4;++fault){
        rejects([&]{CheckpointRun::ethereum(journal,bad,targets,verifier,[&](const auto& batch){
            auto result=runner(batch);
            if(fault==0){result.matches[1]=result.matches[0];}
            if(fault==1){result.matches[0].scalar=UInt256(3);}
            if(fault==2){--result.verified_steps;}
            if(fault==3){result.candidate_count=batch.step_count()+1;result.overflow=true;result.verified_steps=0;result.matches.clear();}
            return result;
        },options);});
        require(journal.results(bad_scope).empty()&&journal.block(bad_scope,UInt256()).covered.empty(),"invalid receipt committed");
    }
    options.candidate_capacity=0;
    rejects([&]{CheckpointRun::ethereum(journal,bad,targets,verifier,runner,options);});
    options.candidate_capacity=64;options.xpoint_steps=1;
    CheckpointRequest request=CheckpointRequest::Run;bool paused=false;unsigned batches=0;
    CheckpointControl controls;
    controls.poll=[&]{return request;};
    controls.notify=[&](CheckpointActivity activity){if(activity==CheckpointActivity::Paused){paused=true;
        require(journal.results(bad_scope).size()==1,"pause did not commit address");}};
    controls.wait=[&]{request=CheckpointRequest::Run;};
    auto resumed=CheckpointRun::ethereum(journal,bad,targets,verifier,[&](const auto& batch){
        auto result=runner(batch);if(++batches==1)request=CheckpointRequest::Pause;return result;
    },options,{},{},controls);
    require(paused&&resumed.complete&&journal.results(bad_scope).size()==2,"paused Ethereum cursor did not resume");
    journal.backup(temporary.path.string()+"/corrupt");
    {Database database(temporary.path.string()+"/corrupt");database.exec("DELETE FROM results WHERE id=(SELECT min(id) FROM results)");}
    Journal corrupt(temporary.path.string()+"/corrupt");rejects([&]{corrupt.check();});
    journal.check();std::cout<<"Ethereum durable relations, replay, pause, receipt rejection and audit passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
