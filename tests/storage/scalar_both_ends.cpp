#include "checkpoint_fixture.h"
#include "checkpoint_data.h"
#include <iostream>
#include <map>
#include <set>
using namespace fixture;
using namespace keyhunt::storage::detail;
using scheduler::ScalarBatchOrder;

template<class Targets,class Create,class Run>
void exercise(const Targets& targets,Create create,Run run,const std::optional<core::ScalarStride>& mapping,
              bool adaptive,unsigned transition,unsigned fault,unsigned bound) {
    Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
    const core::ScalarInterval scalars=mapping?mapping->scalars():core::ScalarInterval(UInt256(101),UInt256(118));
    const auto root=mapping?mapping->indices():scalars;
    const auto project=journal.create_project("both ends recovery");
    const auto scope=create(journal,project,scalars,root.size(),targets,mapping?mapping->stride():UInt256(1),mapping&&mapping->reverse());
    const auto grant=journal.claim(scope,"worker","claim").at(0);
    const auto input=mapping?with_stride(binding(targets),*mapping):binding(targets);
    // Enumerate the expected relations before the checkpoint runner. Subsequent
    // attempts read this table; result verification still runs in production.
    static Bytes previous;static std::map<UInt256,std::vector<uint32_t>> expected;
    auto key=input.configuration;key.insert(key.end(),input.targets.begin(),input.targets.end());
    // Reuse the same independently enumerated relation table across fault cases.
    if(key!=previous){
    expected.clear();
    for(auto i=root.begin();i<root.end();i=i.add(UInt256(1)))for(uint32_t t=0;t<input.count();++t){
        try{input.verify(verifier,i,t);expected[i].push_back(t);}catch(const std::runtime_error&){}
    }
    previous=std::move(key);
    }
    size_t total=0;for(const auto& entry:expected)total+=entry.second.size();require(total>1,"fixture has too few relations");
    std::set<UInt256> missing;
    auto reload=[&]{missing.clear();for(auto i=root.begin();i<root.end();i=i.add(UInt256(1))){
        bool saved=false;for(const auto& gap:journal.block(scope,UInt256()).covered)if(gap.contains(i))saved=true;
        if(!saved)missing.insert(i);
    }};reload();
    CheckpointOptions options;options.xpoint_steps=11;options.candidate_capacity=bound;options.checkpoint_seconds=0;
    options.work_unit_seconds=adaptive?60:0;
    options.scalar_batch_order=transition==1?ScalarBatchOrder::Forward:ScalarBatchOrder::BothEnds;
    unsigned accepted=0,overflows=0;bool faulted=false,cleaned=false;
    const auto runner=[&](const scheduler::KernelBatch& batch){
        backend::XPointResult result{batch,{}};result.device_steps=batch.step_count();
        for(uint64_t j=0;j<batch.step_count();++j){const auto i=batch.coordinate_at(j);
            require(missing.count(i),"submitted previously saved coverage");
            for(auto t:expected[i])result.matches.push_back({i,t});
        }
        result.candidate_count=result.matches.size();result.overflow=result.candidate_count>options.candidate_capacity;
        if(result.overflow){++overflows;result.matches.clear();}
        else{
            // Fault after execution but before any receipt. This suffix must be
            // rediscovered by the next invocation's saved complement.
            if(fault==0 && accepted==3 && !faulted){faulted=true;throw std::runtime_error("before receipt");}
            result.verified_steps=result.device_steps;++accepted;
            for(uint64_t j=0;j<batch.step_count();++j)missing.erase(batch.coordinate_at(j));
        }
        return result;
    };
    rejects([&]{run(journal,grant,targets,verifier,runner,options,
        [&](const auto&,size_t,double){if(fault==1 && accepted==4){faulted=true;throw std::runtime_error("lost receipt reply");}},
        [&]{cleaned=true;},CheckpointControl{});});
    require(faulted&&cleaned&&overflows,"fault/overflow fixture did not trigger");reload();
    const auto left=missing.size();require(left>0 && left<root.size().to_uint64(),"no partial durable progress");
    const auto retained=root.size().subtract(UInt256(left));
    options.scalar_batch_order=transition==0?ScalarBatchOrder::Forward:ScalarBatchOrder::BothEnds;
    options.xpoint_steps=1;options.candidate_capacity=1024;
    CheckpointRequest request=CheckpointRequest::Run;bool paused=false;unsigned planned=0;
    CheckpointControl controls;controls.poll=[&]{return request;};controls.wait=[&]{request=CheckpointRequest::Run;};
    controls.notify=[&](CheckpointActivity activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
    controls.work_unit=[&](const auto& work){++planned;require(root.contains(work),"owner escapes block");};
    const auto result=run(journal,grant,targets,verifier,[&](const auto& batch){auto r=runner(batch);
        if(!paused)request=CheckpointRequest::Pause;
        return r;},options,CheckpointObserver{},CheckpointCleanup{},controls);
    require(result.complete&&result.resumed_scalars==retained&&result.computed_scalars==UInt256(left)&&missing.empty(),"restart did not cover exact complement");
    require(paused || left==1,"partial work failed to pause");
    require(result.work_units==planned,"owner announcement count differs");
    const auto rows=journal.results(scope,0,1000);require(rows.size()==total,"lost or duplicated results");
    for(const auto& row:rows)input.verify(verifier,row.scalar,row.target);
    options.scalar_batch_order=ScalarBatchOrder::BothEnds;
    const auto done=run(journal,grant,targets,verifier,[](const auto&)->backend::XPointResult{throw std::runtime_error("finished job ran");},options,
        CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});
    require(done.batches==0&&done.resumed_scalars==root.size(),"completed retry changed coverage");
    journal.backup(temporary.path.string()+"/snapshot");Journal backup(temporary.path.string()+"/snapshot");backup.check();
    require(backup.results(scope,0,1000).size()==total,"backup lost results");journal.check();
}
int main(){try{
    unsigned cases=0;core::XPointVerifier verifier;
    for(unsigned kind=0;kind<5;++kind){
        const bool orbit=kind>=3,reverse=kind==2||kind==4;
        const std::optional<core::ScalarStride> mapping=kind?std::make_optional(core::ScalarStride({UInt256(101),UInt256(164)},UInt256(7),reverse,orbit)):std::nullopt;
        std::vector<core::XPointBytes> xv;std::vector<core::Hash160Target> hv;
        std::vector<core::EthereumTarget> ev;std::vector<core::VanityTarget> vv;
        const core::ScalarInterval root=mapping?mapping->indices():core::ScalarInterval(UInt256(101),UInt256(118));
        for(auto i=root.begin();i<root.end();i=i.add(UInt256(1))){
            const auto pub=verifier.derive(mapping?mapping->scalar(i):i);core::XPointBytes x{};std::copy_n(pub.begin()+1,32,x.begin());
            xv.push_back(x);ev.push_back(core::ethereum_target(pub));
            for(unsigned tag:{1,2}){hv.push_back(core::hash160_target(pub,tag));vv.push_back(core::vanity_target(core::bitcoin_address(pub,tag),tag));}
        }
        const core::XPointTargets xt(xv);const core::Hash160Targets ht(hv);const core::EthereumTargets et(ev);const core::VanityTargets vt(vv);
        for(bool adaptive:{false,true})for(unsigned transition=0;transition<3;++transition)for(unsigned fault=0;fault<2;++fault){
            exercise(xt,orbit?CheckpointRun::create_orbit_xpoint:CheckpointRun::create_xpoint,CheckpointRun::xpoint,mapping,adaptive,transition,fault,1);
            exercise(ht,orbit?CheckpointRun::create_orbit_hash160:CheckpointRun::create_hash160,CheckpointRun::hash160,mapping,adaptive,transition,fault,2);
            exercise(et,orbit?CheckpointRun::create_orbit_ethereum:CheckpointRun::create_ethereum,CheckpointRun::ethereum,mapping,adaptive,transition,fault,1);
            exercise(vt,orbit?CheckpointRun::create_orbit_vanity:CheckpointRun::create_vanity,CheckpointRun::vanity,mapping,adaptive,transition,fault,vt.max_matches_per_scalar());
            cases+=4;
        }
    }
    std::cout<<"Passed "<<cases<<" scalar both-ends recovery combinations\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
