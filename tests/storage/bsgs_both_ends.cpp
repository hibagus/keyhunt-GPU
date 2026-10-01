#include "checkpoint_fixture.h"
#include <iostream>
#include <set>
using namespace fixture;
int main(){try{
    for(const auto order:{core::BsgsTileOrder::Forward,core::BsgsTileOrder::Reverse,core::BsgsTileOrder::BothEnds})for(unsigned seconds:{0U,180U}){
        Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
        const auto targets=b_targets(verifier,{101,102,120,137,156,195,197});const auto table=bsgs::Table::build(7);
        const auto project=journal.create_project("both-ends recovery");
        const auto scope=CheckpointRun::create_bsgs(journal,project,{UInt256(101),UInt256(198)},UInt256(97),targets,table);
        const auto grant=journal.claim(scope,"worker","claim").at(0);
        CheckpointOptions options;options.giant_steps=2;options.target_batch=7;options.candidate_capacity=1;
        options.checkpoint_seconds=0;options.bsgs_tile_order=core::BsgsTileOrder::BothEnds;
        bool acknowledged=false,cleaned=false;unsigned overflows=0;
        rejects([&]{CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            require(batch.interval().begin()==UInt256(101)&&batch.interval().end()==UInt256(115),"both-ends advanced during target replay");
            auto result=execute(batch,targets,verifier,1);overflows+=result.overflow;return result;
        },options,[&](const auto& covered,size_t matches,double){
            require(covered.empty()&&matches,"partial target group certified a tile");
            acknowledged=true;throw std::runtime_error("lost acknowledgment");
        },[&]{cleaned=true;});});
        require(acknowledged&&cleaned&&overflows,"subgroup replay fixture did not run");
        require(journal.block(scope,UInt256()).covered.empty()&&!journal.results(scope).empty(),"partial result durability changed");
        options.giant_steps=3;options.target_batch=2;options.candidate_capacity=2;
        options.bsgs_tile_order=order;options.work_unit_seconds=seconds;
        UInt256 low(101),high(198);std::optional<ScalarInterval> previous;unsigned tiles=0;
        bool requested=false,paused=false;CheckpointRequest request=CheckpointRequest::Run;
        CheckpointControl control;control.poll=[&]{return request;};control.wait=[&]{request=CheckpointRequest::Run;};
        control.notify=[&](auto activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
        const auto summary=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            const auto& tile=batch.interval();
            if(!previous||tile.begin()!=previous->begin()||tile.end()!=previous->end()){
                const bool upper=order==core::BsgsTileOrder::Reverse||(order==core::BsgsTileOrder::BothEnds&&tiles%2);
                require(tile.begin()>=low&&tile.end()<=high,"overlapping tile fronts");
                require(upper?tile.end()==high:tile.begin()==low,"wrong alternating endpoint");
                if(upper)high=tile.begin();else low=tile.end();
                previous=tile;++tiles;
            }
            auto result=execute(batch,targets,verifier,2);
            if(!requested){requested=true;request=CheckpointRequest::Pause;}return result;
        },options,{},{},control);
        require(paused&&summary.complete&&low==high&&summary.computed_scalars==UInt256(97),"both-ends resume coverage changed");
        // With adaptation, the second unit covers the remaining range. Both-ends
        // must still alternate within that unit, including its final short tile.
        require(tiles>=5&&(!seconds||summary.work_units<tiles),"adaptive multi-tile unit not exercised");
        std::set<UInt256> expected{UInt256(101),UInt256(102),UInt256(120),UInt256(137),UInt256(156),UInt256(195),UInt256(197)};
        require(journal.results(scope).size()==expected.size(),"replayed subgroup duplicated results");
        for(const auto& row:journal.results(scope))require(expected.erase(row.scalar),"unexpected result");
        options.bsgs_tile_order=core::BsgsTileOrder::BothEnds;
        const auto done=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[](const auto&)->backend::BsgsSearchResult{
            throw std::runtime_error("completed grant executed");},options);
        require(done.complete&&!done.batches&&done.resumed_scalars==UInt256(97),"finished grant replayed");
        journal.check();
    }
    std::cout<<"Both-ends subgroup replay, adaptive tile alternation, pause and all-order restart passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
