#include "checkpoint_fixture.h"
#include <iostream>
#include <set>
using namespace fixture;
int main(){try{
    for(bool resume_reverse:{false,true}){
        Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
        const auto targets=b_targets(verifier,{101,102,120,137,156,157,1000});const auto table=bsgs::Table::build(7);
        const auto project=journal.create_project("reverse BSGS recovery");
        const auto scope=CheckpointRun::create_bsgs(journal,project,{UInt256(101),UInt256(158)},UInt256(57),targets,table);
        const auto grant=journal.claim(scope,"worker","claim").at(0);
        const std::vector<core::XPointMatch> prior_matches;
        const auto before=journal.block(scope,UInt256());UInt256 retained;
        for(const auto& span:before.covered)retained=retained.add(span.size());
        CheckpointOptions options;options.giant_steps=2;options.target_batch=7;options.candidate_capacity=1;
        options.checkpoint_seconds=0;options.bsgs_reverse_tiles=true;
        bool committed=false,cleaned=false;unsigned overflows=0;
        rejects([&]{CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            require(batch.interval().begin()==UInt256(144)&&batch.interval().end()==UInt256(158),"reverse did not start at highest gap");
            auto result=execute(batch,targets,verifier,1);overflows+=result.overflow;return result;
        },options,[&](const auto& covered,size_t matches,double){
            require(covered.empty()&&matches,"partial target group credited coverage");
            committed=true;journal.check();throw std::runtime_error("lost acknowledgment");
        },[&]{cleaned=true;});});
        require(committed&&cleaned&&overflows,"partial reverse overflow/retry was not exercised");
        require(journal.block(scope,UInt256()).covered.size()==before.covered.size(),"partial tile coverage changed");
        require(journal.results(scope).size()>prior_matches.size(),"committed subgroup match was lost");
        options.giant_steps=3;options.target_batch=2;options.candidate_capacity=2;options.bsgs_reverse_tiles=resume_reverse;
        CheckpointRequest request=CheckpointRequest::Run;bool paused=false,requested=false;
        CheckpointControl control;control.poll=[&]{return request;};control.wait=[&]{request=CheckpointRequest::Run;};
        control.notify=[&](auto activity){if(activity==CheckpointActivity::Paused){paused=true;journal.check();}};
        std::optional<ScalarInterval> previous;
        const auto summary=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            const auto& tile=batch.interval();bool contained=false;
            for(const auto& gap:before.remaining)contained|=gap.contains(tile);
            require(contained,"tile crossed saved coverage");
            if(previous && (tile.begin()!=previous->begin()||tile.end()!=previous->end()))
                require(resume_reverse?tile.end()<=previous->begin():tile.begin()>=previous->end(),"wrong gap or tile order");
            previous=tile;auto result=execute(batch,targets,verifier,2);
            if(!requested){request=CheckpointRequest::Pause;requested=true;}return result;
        },options,{}, {},control);
        require(paused&&summary.complete&&summary.resumed_scalars==retained&&summary.computed_scalars.add(retained)==UInt256(57),"resume changed exact coverage");
        std::set<UInt256> expected{UInt256(101),UInt256(102),UInt256(120),UInt256(137),UInt256(156),UInt256(157)};
        require(journal.results(scope).size()==expected.size(),"subgroup replay duplicated results");
        for(const auto& row:journal.results(scope))require(expected.erase(row.scalar),"unexpected recovered result");
        options.bsgs_reverse_tiles=!resume_reverse;
        const auto done=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[](const auto&)->backend::BsgsSearchResult{
            throw std::runtime_error("completed grant executed");},options);
        require(done.complete&&done.batches==0&&done.resumed_scalars==UInt256(57),"direction switch recomputed finished work");
        // Explicit forward is still unsupported on a non-BSGS runner.
        const auto xs=x_targets(verifier,{1});options.bsgs_reverse_tiles=false;
        const auto scalar_scope=CheckpointRun::create_xpoint(journal,project,{UInt256(1),UInt256(3)},UInt256(2),xs);
        const auto scalar_grant=journal.claim(scalar_scope,"worker","scalar").at(0);
        bool executed=false;
        rejects([&]{CheckpointRun::xpoint(journal,scalar_grant,xs,verifier,[&](const auto&)->backend::XPointResult{executed=true;throw std::runtime_error("executed");},options);});
        require(!executed,"unsupported tile-order reached scalar executor");
        require(journal.block(scalar_scope,UInt256()).covered.empty(),"unsupported option changed coverage");
        journal.check();
    }
    std::cout<<"Reverse BSGS partial-target replay, pause and direction switches passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
