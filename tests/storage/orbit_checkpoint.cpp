#include "checkpoint_fixture.h"
#include "checkpoint_data.h"
#include <iostream>
using namespace fixture;
using namespace keyhunt::storage::detail;
template<class Targets,class Create,class Run>
void exercise(const Targets& targets,unsigned bound,Create create,Run run){
    Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
    const core::ScalarStride mapping({UInt256(101),UInt256(332)},UInt256(7),true,true);
    const auto project=journal.create_project("orbit recovery");
    const auto scope=create(journal,project,mapping.scalars(),UInt256(198),targets,UInt256(7),true);
    const auto input=with_stride(binding(targets),mapping);const auto manifest=journal.manifest(scope);
    require(journal.stride_mapping(scope)==std::optional<core::ScalarStride>(mapping),"mapping was not persisted");
    require(manifest.root.size()==UInt256(198)&&input.configuration.size()==146,"binding domain mismatch");
    require(decode_binding(manifest,input.configuration,input.targets).stride_mapping==input.stride_mapping,"binding round trip mismatch");
    // Reject malformed configuration even when its digest is updated by a sender.
    for(unsigned fault=0;fault<10;++fault){
        auto bad=input.configuration;auto m=manifest;
        if(fault==0)bad[8]=1;
        if(fault==1)bad.pop_back();
        if(fault==2)bad.push_back(0);
        if(fault==3)bad[17]=1;
        if(fault==4)bad[18]=1;
        if(fault==5)bad.back()=1;
        if(fault==6)bad.back()=0;
        if(fault==7)m.root={UInt256(2),UInt256(35)};
        if(fault==9)bad[8]=6;
        if(fault==8)bad[81]=0; // zero scalar origin
        const auto hash=digest(bad);std::copy(hash.begin(),hash.end(),m.algorithm.begin());
        rejects([&]{decode_binding(m,bad,input.targets);});
    }
    const auto grant=journal.claim(scope,"worker","claim").at(0);
    CheckpointOptions options;options.xpoint_steps=33;options.candidate_capacity=bound;options.checkpoint_seconds=0;
    const auto runner=[&](const auto& batch){
        backend::XPointResult result{batch,{}};result.device_steps=batch.step_count();
        for(uint64_t i=0;i<batch.step_count();++i)for(uint32_t t=0;t<input.count();++t){
            // This CPU runner exercises journal acceptance with exact coordinates.
            // Separate pinned-oracle tests establish device arithmetic parity.
            try{input.verify(verifier,batch.coordinate_at(i),t);result.matches.push_back({batch.coordinate_at(i),t});}
            catch(const std::runtime_error&){}
        }
        result.candidate_count=result.matches.size();result.overflow=result.candidate_count>options.candidate_capacity;
        if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;return result;
    };
    auto requested=options;requested.stride=UInt256(8);
    rejects([&]{run(journal,grant,targets,verifier,runner,requested,CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});});
    require(journal.block(scope,UInt256()).covered.empty(),"wrong stride mutated coverage");
    requested=options;requested.reverse=false;
    rejects([&]{run(journal,grant,targets,verifier,runner,requested,CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});});
    require(journal.block(scope,UInt256()).covered.empty(),"wrong order mutated coverage");
    requested=options;requested.orbit=false;
    rejects([&]{run(journal,grant,targets,verifier,runner,requested,CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});});
    require(journal.block(scope,UInt256()).covered.empty(),"wrong expansion mutated coverage");
    // Forward orbit is valid, but cannot retain the reverse orbit job ID.
    auto forward=input.configuration;forward[8]=4;
    rejects([&]{decode_binding(manifest,forward,input.targets);});
    const core::ScalarStride unit({UInt256(101),UInt256(134)},UInt256(1),true,true);
    const auto unit_scope=create(journal,project,unit.scalars(),UInt256(198),targets,UInt256(1),true);
    require(journal.stride_mapping(unit_scope)==std::optional<core::ScalarStride>(unit),"reverse unit stride lost binding");
    bool committed=false,cleaned=false;
    rejects([&]{run(journal,grant,targets,verifier,runner,options,
        [&](const auto&,size_t,double){committed=true;journal.check();throw std::runtime_error("lost acknowledgement");},
        [&]{cleaned=true;},CheckpointControl{});});
    require(committed&&cleaned&&!journal.results(scope).empty(),"durable index receipt missing");
    const auto retained=journal.block(scope,UInt256()).covered.at(0).size();
    require(retained<UInt256(198),"overflow credited full attempt");
    options.xpoint_steps=5;options.stride=UInt256(7);options.reverse=true;
    CheckpointRequest request=CheckpointRequest::Run;bool paused=false;
    CheckpointControl controls;controls.poll=[&]{return request;};
    controls.notify=[&](CheckpointActivity activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
    controls.wait=[&]{request=CheckpointRequest::Run;};
    unsigned batches=0;
    const auto summary=run(journal,grant,targets,verifier,[&](const auto& batch){
        auto result=runner(batch);if(!result.overflow&&++batches==1)request=CheckpointRequest::Pause;return result;
    },options,CheckpointObserver{},CheckpointCleanup{},controls);
    require(paused&&summary.complete&&summary.resumed_scalars==retained&&summary.computed_scalars.add(retained)==UInt256(198),"resume lost candidate indices");
    for(const auto& row:journal.results(scope)){require(mapping.indices().contains(row.scalar),"stored coordinate is not an index");input.verify(verifier,row.scalar,row.target);}
    const auto done=run(journal,grant,targets,verifier,[](const auto&)->backend::XPointResult{throw std::runtime_error("finished work executed");},options,
        CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});
    require(done.resumed_scalars==UInt256(198)&&done.batches==0,"finished replay changed coverage");
    // A private scalar placed in the receipt's index field must never commit.
    const auto bad_scope=create(journal,project,mapping.scalars(),UInt256(199),targets,UInt256(7),true);
    const auto bad_grant=journal.claim(bad_scope,"worker","bad").at(0);options.candidate_capacity=1024;
    rejects([&]{run(journal,bad_grant,targets,verifier,[&](const auto& batch){auto result=runner(batch);
        result.matches[0].scalar=mapping.indices().end();return result;
    },options,CheckpointObserver{},CheckpointCleanup{},CheckpointControl{});});
    require(journal.results(bad_scope).empty()&&journal.block(bad_scope,UInt256()).covered.empty(),"scalar/index confusion committed");
    journal.backup(temporary.path.string()+"/snapshot");Journal snapshot(temporary.path.string()+"/snapshot");snapshot.check();
    require(snapshot.stride_mapping(scope)==input.stride_mapping,"backup lost mapping");journal.check();
}
int main(){try{
    core::XPointVerifier verifier;std::vector<core::XPointBytes> xv;std::vector<core::Hash160Target> hv;
    std::vector<core::EthereumTarget> ev;std::vector<core::VanityTarget> vv;
    for(unsigned i:{0U,32U})for(unsigned variant=0;variant<6;++variant){const auto pub=verifier.derive(core::scalar_orbit(UInt256(101+7*i),variant));core::XPointBytes x{};
        std::copy_n(pub.begin()+1,32,x.begin());xv.push_back(x);ev.push_back(core::ethereum_target(pub));
        for(unsigned tag:{1,2}){hv.push_back(core::hash160_target(pub,tag));vv.push_back(core::vanity_target(core::bitcoin_address(pub,tag),tag));}}
    exercise(core::XPointTargets(xv),1,CheckpointRun::create_orbit_xpoint,CheckpointRun::xpoint);
    exercise(core::Hash160Targets(hv),2,CheckpointRun::create_orbit_hash160,CheckpointRun::hash160);
    exercise(core::EthereumTargets(ev),1,CheckpointRun::create_orbit_ethereum,CheckpointRun::ethereum);
    const core::VanityTargets vt(vv);exercise(vt,vt.max_matches_per_scalar(),CheckpointRun::create_orbit_vanity,CheckpointRun::vanity);
    const auto mt=binding(core::MinikeyTargets({core::minikey_target(22,hv[0])}));
    rejects([&]{with_stride(mt,core::ScalarStride({UInt256(1),UInt256(4)},UInt256(2)));});
    std::cout<<"Four-family orbit binding, overflow, pause, durable replay and receipt rejection passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
