#include "checkpoint_fixture.h"
#include <iostream>
#include <set>
using namespace fixture;
int main(){try{
    using core::MinikeyOrder;core::XPointVerifier verifier;
    for(const std::string key:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
        const auto begin=core::minikey_ordinal(key),end=begin.add(UInt256(4097));const unsigned length=key.size();
        std::vector<core::MinikeyTarget> values;std::set<UInt256> wanted;
        for(auto at=begin;at<end;at=at.add(UInt256(1))){
            const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
            wanted.insert(at);const auto point=verifier.derive(*scalar);
            for(uint8_t tag:{1,2})values.push_back(core::minikey_target(length,core::hash160_target(point,tag)));
        }
        const core::MinikeyTargets targets(values);
        for(const auto order:{MinikeyOrder::Forward,MinikeyOrder::Reverse,MinikeyOrder::BothEnds,MinikeyOrder::Dance,MinikeyOrder::RandomWindow})
        for(unsigned seconds:{0U,180U})for(unsigned fault:{0U,1U}){
            Temporary temporary;Journal journal(temporary.path.string());const auto project=journal.create_project("Random ordinal recovery");
            const auto scope=CheckpointRun::create_minikeys(journal,project,{begin,end},UInt256(4097),targets);
            const auto grant=journal.claim(scope,"owner","claim").at(0);
            CheckpointOptions options;options.minikey_order=MinikeyOrder::RandomWindow;
            options.minikey_random_window=core::MinikeyRandomWindow{UInt256(42),4};
            options.xpoint_steps=1024;options.candidate_capacity=2;options.checkpoint_seconds=0;
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
            std::set<UInt256> missing;for(auto at=begin;at<end;at=at.add(UInt256(1)))missing.insert(at);
            // The independently specified seed-42 permutation starts with tiles
            // 3 then 1. Overflow must finish tile 3's suffix before visiting 1.
            auto cursor=begin.add(UInt256(3072));auto tile_end=begin.add(UInt256(4096));
            unsigned tiles=0,overflows=0,accepted=0;bool stop=false,cleaned=false;
            rejects([&]{CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
                require(!batch.ordinal_reverse()&&batch.interval().begin()==cursor&&batch.interval().end()<=tile_end,"overflow moved the shuffled tile");
                auto result=execute(batch);
                if(result.overflow)++overflows;
                else{
                    for(uint64_t i=0;i<batch.step_count();++i)require(missing.erase(batch.ordinal_at(i)),"initial duplicate ordinal");
                    ++accepted;stop=fault?tiles==1:accepted==1;cursor=batch.interval().end();
                    if(cursor==tile_end){++tiles;cursor=begin.add(UInt256(1024));tile_end=begin.add(UInt256(2048));}
                }
                return result;
            },options,[&](const auto&,size_t,double){if(stop)throw std::runtime_error("lost durable acknowledgment");},[&]{cleaned=true;});});
            const auto retained=journal.block(scope,UInt256());const auto resumed=UInt256(4097-missing.size());
            require(cleaned&&overflows&&accepted&&retained.covered.size()==(fault?2U:1U),"expected interrupted window receipts missing");
            require(fault?resumed>UInt256(1024):resumed==UInt256(1),"first overflow retry did not accept one ordinal");
            options.minikey_order=order;options.minikey_random_window.reset();
            if(order==MinikeyOrder::RandomWindow)options.minikey_random_window=core::MinikeyRandomWindow{UInt256(0x1234),3};
            options.xpoint_steps=17;options.work_unit_seconds=seconds;
            bool requested=false,paused=false,grew=false;CheckpointRequest request=CheckpointRequest::Run;
            CheckpointControl control;control.poll=[&]{return request;};control.wait=[&]{request=CheckpointRequest::Run;};
            control.notify=[&](auto activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
            const auto summary=CheckpointRun::minikeys(journal,grant,targets,verifier,[&](const auto& batch){
                grew|=batch.work().interval().size()>UInt256(17);auto result=execute(batch);
                if(!result.overflow)for(uint64_t i=0;i<batch.step_count();++i)require(missing.erase(batch.ordinal_at(i)),"replayed accepted ordinal");
                if(!requested){requested=true;request=CheckpointRequest::Pause;}return result;
            },options,{},{},control);
            require(summary.complete&&missing.empty()&&paused&&summary.resumed_scalars==resumed&&summary.computed_scalars==UInt256(4097).subtract(resumed),"restart complement differs");
            require(!seconds||grew,"adaptive units never grew");
            const auto rows=journal.results(scope);require(rows.size()==wanted.size()*2,"encoding result union changed");
            for(const auto& row:rows)require(wanted.count(row.scalar),"unexpected result ordinal");
            options.minikey_order=MinikeyOrder::RandomWindow;options.minikey_random_window=core::MinikeyRandomWindow{UInt256(7),256};
            const auto done=CheckpointRun::minikeys(journal,grant,targets,verifier,[](const auto&)->backend::MinikeysResult{throw std::runtime_error("completed grant executed");},options);
            require(done.complete&&!done.batches&&done.resumed_scalars==UInt256(4097),"completed retry changed coverage");journal.check();
        }
    }
    std::cout<<"Forty random-window replay/restart combinations, both lengths, all orders, adaptive work and pause passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
