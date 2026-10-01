#include "checkpoint_fixture.h"
#include <iostream>
#include <set>
using namespace fixture;
int main(){try{
    for(const bool middle_fault:{false,true})
    for(const auto order:{core::BsgsTileOrder::Forward,core::BsgsTileOrder::Reverse,core::BsgsTileOrder::BothEnds,core::BsgsTileOrder::Dance})
    for(unsigned seconds:{0U,180U}){
        Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
        const std::set<UInt256> wanted{UInt256(101),UInt256(102),UInt256(120),UInt256(137),UInt256(149),UInt256(150),UInt256(156),UInt256(195),UInt256(197)};
        const auto targets=b_targets(verifier,{101,102,120,137,149,150,156,195,197});const auto table=bsgs::Table::build(7);
        const auto project=journal.create_project("dance recovery");
        const auto scope=CheckpointRun::create_bsgs(journal,project,{UInt256(101),UInt256(198)},UInt256(97),targets,table);
        const auto grant=journal.claim(scope,"worker","claim").at(0);
        CheckpointOptions options;options.giant_steps=2;options.target_batch=9;options.candidate_capacity=1;
        options.checkpoint_seconds=0;options.bsgs_tile_order=core::BsgsTileOrder::Dance;
        bool acknowledged=false,cleaned=false,in_fault_tile=false;unsigned overflows=0;
        const auto fault_begin=UInt256(middle_fault?149:101);
        rejects([&]{CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            in_fault_tile=batch.interval().begin()==fault_begin;
            require(batch.interval().begin()==UInt256(101)||batch.interval().begin()==UInt256(184)||in_fault_tile,"dance advanced past interrupted tile");
            auto result=execute(batch,targets,verifier,1);overflows+=result.overflow;return result;
        },options,[&](const auto& covered,size_t matches,double){
            if(in_fault_tile && matches){
                require(covered.empty(),"partial target group certified a tile");
                acknowledged=true;throw std::runtime_error("lost subgroup acknowledgment");
            }
        },[&]{cleaned=true;});});
        require(acknowledged&&cleaned&&overflows,"subgroup replay fixture did not run");
        const auto saved=journal.block(scope,UInt256());
        require(saved.covered.size()==(middle_fault?2U:0U)&&!journal.results(scope).empty(),"partial result or prior coverage lost");
        // Enumerate this small fixture independently. Membership erasure catches
        // overlaps, gaps and replay of already certified endpoint tiles.
        std::set<UInt256> missing;
        for(const auto& gap:saved.remaining)for(auto n=gap.begin();n<gap.end();n=n.add(UInt256(1)))missing.insert(n);
        const auto pivot=missing.begin()->add(missing.rbegin()->add(UInt256(1)).subtract(*missing.begin()).divmod(UInt256(2)).first);
        options.giant_steps=3;options.target_batch=2;options.candidate_capacity=2;
        options.bsgs_tile_order=order;options.work_unit_seconds=seconds;
        std::optional<ScalarInterval> previous;unsigned tiles=0;
        bool requested=false,paused=false;CheckpointRequest request=CheckpointRequest::Run;
        CheckpointControl control;control.poll=[&]{return request;};control.wait=[&]{request=CheckpointRequest::Run;};
        control.notify=[&](auto activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
        const auto summary=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            const auto& tile=batch.interval();
            if(!previous||tile.begin()!=previous->begin()||tile.end()!=previous->end()){
                require(!missing.empty(),"executed after exact completion");
                const bool high=order==core::BsgsTileOrder::Reverse || (order==core::BsgsTileOrder::BothEnds&&tiles%2==1) || (order==core::BsgsTileOrder::Dance&&tiles%3==1);
                auto low=missing.begin();
                if(order==core::BsgsTileOrder::Dance&&tiles%3==2){low=missing.lower_bound(pivot);if(low==missing.end())low=missing.begin();}
                require(high?tile.end()==missing.rbegin()->add(UInt256(1)):tile.begin()==*low,"wrong dance front");
                if(order==core::BsgsTileOrder::Dance)require(!(tile.begin()<pivot&&pivot<tile.end()),"crossed fixed pivot");
                for(auto n=tile.begin();n<tile.end();n=n.add(UInt256(1)))require(missing.erase(n),"tile repeated a scalar");
                previous=tile;++tiles;
            }
            auto result=execute(batch,targets,verifier,2);
            if(!requested){requested=true;request=CheckpointRequest::Pause;}return result;
        },options,{},{},control);
        require(paused&&summary.complete&&missing.empty(),"dance did not complete complement");
        require(summary.resumed_scalars==UInt256(middle_fault?28:0)&&summary.computed_scalars==UInt256(middle_fault?69:97),"resume counts changed");
        require(!seconds||summary.work_units<tiles,"adaptive multi-tile unit not exercised");
        auto expected=wanted;require(journal.results(scope).size()==expected.size(),"replayed subgroup duplicated results");
        for(const auto& row:journal.results(scope))require(expected.erase(row.scalar),"unexpected result");
        options.bsgs_tile_order=core::BsgsTileOrder::Dance;
        const auto done=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[](const auto&)->backend::BsgsSearchResult{
            throw std::runtime_error("completed grant executed");},options);
        require(done.complete&&!done.batches&&done.resumed_scalars==UInt256(97),"finished grant replayed");
        journal.check();
    }
    std::cout<<"Dance endpoint/middle subgroup replay, adaptive units, pause and all-order restart passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
