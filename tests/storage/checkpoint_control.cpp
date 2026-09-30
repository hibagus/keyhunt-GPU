#include "checkpoint_fixture.h"
#include <iostream>
using namespace fixture;

int main(){
    try{
        Temporary tmp;Journal journal(tmp.path.string());core::XPointVerifier verifier;
        const auto project=journal.create_project("C14 controls");
        const auto absent=x_targets(verifier,{1000});
        CheckpointOptions options;options.xpoint_steps=4;options.checkpoint_seconds=60;
        auto make=[&](uint64_t start,uint64_t size){
            auto scope=CheckpointRun::create_xpoint(journal,project,
                ScalarInterval(UInt256(start),UInt256(start+size)),UInt256(size),absent);
            return journal.claim(scope,"owner","claim").at(0);
        };
        auto grant=make(1,20);
        unsigned launches=0,pauses=0,idle=0;CheckpointRequest request=CheckpointRequest::Run;
        CheckpointControl control;
        control.poll=[&]{return request;};
        control.notify=[&](CheckpointActivity activity){
            if(activity!=CheckpointActivity::Paused)return;
            ++pauses;const auto saved=journal.block(grant.scope,grant.block);
            require(saved.state=="in_progress" && saved.started && saved.assignment->generation==grant.generation,"pause released ownership");
            require(saved.covered.size()==1 && saved.covered[0].size()==UInt256(4*launches),"pause lost pending coverage");
            // An online snapshot while the owner is paused must contain the
            // just-committed frontier and must remain sealed after restore.
            if(pauses==1){
                const auto backup=(tmp.path/"snapshot").string();
                journal.backup(backup);Journal snapshot(backup);snapshot.check();
                require(snapshot.block(grant.scope,grant.block).covered[0].size()==UInt256(4),"backup missed pause commit");
                const auto restored=(tmp.path/"restored").string();
                Journal::restore(backup,restored);Journal copy(restored);copy.check();
                rejects([&]{CheckpointRun::xpoint(copy,grant,absent,verifier,[&](const auto& b){return execute(b,absent,verifier,1024);},options);});
            }
            rejects([&]{CheckpointRun::xpoint(journal,grant,absent,verifier,[&](const auto& b){return execute(b,absent,verifier,1024);},options);});
        };
        control.wait=[&]{
            ++idle;require(launches==pauses,"a paused owner submitted work");
            // Stop from the second pause; the first resume continues its cursor.
            if(idle%3==0)request=pauses==1?CheckpointRequest::Run:CheckpointRequest::Stop;
        };
        const auto stopped=CheckpointRun::xpoint(journal,grant,absent,verifier,[&](const auto& b){
            ++launches;request=CheckpointRequest::Pause;return execute(b,absent,verifier,1024);
        },options,{}, {},control);
        require(!stopped.complete && pauses==2 && idle==6 && stopped.checkpoints==2,"pause/stop state machine");
        const auto resumed=CheckpointRun::xpoint(journal,grant,absent,verifier,[&](const auto& b){
            require(b.interval().begin()>=UInt256(9),"restart repeated durable coverage");
            return execute(b,absent,verifier,1024);
        },options);
        require(resumed.complete && resumed.resumed_scalars==UInt256(8) && resumed.computed_scalars==UInt256(12),"restart complement");

        // Stop before the first submission, including an already paused startup.
        grant=make(30,8);request=CheckpointRequest::Stop;
        const auto zero=CheckpointRun::xpoint(journal,grant,absent,verifier,
            [](const auto&)->backend::XPointResult{throw std::runtime_error("stopped owner launched");},options,{}, {},{[&]{return request;},{},{}});
        require(!zero.complete && zero.batches==0 && zero.checkpoints==0,"zero-work stop");

        // A deadline elapsed during idle and a control-plane transfer both fence
        // resume before the retained cursor can reach the runner.
        for(bool transfer:{false,true}){
            int64_t now=1000;Journal clocked(tmp.path.string(),[&]{return now;});
            auto scope=CheckpointRun::create_xpoint(clocked,project,
                ScalarInterval(UInt256(transfer?60:50),UInt256(transfer?68:58)),UInt256(8),absent);
            const auto g=clocked.claim(scope,"owner","fence",{},10).at(0);
            request=CheckpointRequest::Run;unsigned calls=0;
            CheckpointControl fence{[&]{return request;},[&](CheckpointActivity activity){
                if(activity==CheckpointActivity::Paused){
                    if(transfer)clocked.recover(scope,UInt256(),"new-owner","transfer",true);
                    else now=1010;
                }
            },[&]{request=CheckpointRequest::Run;}};
            rejects([&]{CheckpointRun::xpoint(clocked,g,absent,verifier,[&](const auto& b){
                ++calls;request=CheckpointRequest::Pause;return execute(b,absent,verifier,1024);
            },options,{}, {},fence);});
            require(calls==1 && clocked.block(scope,UInt256()).covered[0].size()==UInt256(4),"fenced resume launched or lost checkpoint");
        }

        const auto targets=b_targets(verifier,{101,103,105});
        const auto table=bsgs::Table::build(3);
        auto scope=CheckpointRun::create_bsgs(journal,project,ScalarInterval(UInt256(101),UInt256(110)),UInt256(9),targets,table);
        const auto bg=journal.claim(scope,"owner","bsgs").at(0);
        options.target_batch=1;options.giant_steps=3;launches=0;request=CheckpointRequest::Run;
        CheckpointControl partial{[&]{return request;},[&](CheckpointActivity activity){
            if(activity==CheckpointActivity::Paused){
                require(journal.block(scope,UInt256()).covered.empty(),"partial target pause credited full tile");
                require(journal.results(scope).size()==1,"partial target match not durable");
            }
        },[&]{request=CheckpointRequest::Run;}};
        auto b=CheckpointRun::bsgs(journal,bg,targets,table,verifier,[&](const auto& batch){
            require(batch.first_target()==launches,"live resume repeated subgroup");
            if(++launches==1)request=CheckpointRequest::Pause;
            return execute(batch,targets,verifier,1024);
        },options,{}, {},partial);
        require(b.complete && launches==3 && journal.results(scope).size()==3,"live BSGS resume");

        scope=CheckpointRun::create_bsgs(journal,project,ScalarInterval(UInt256(100),UInt256(110)),UInt256(10),targets,table);
        const auto bg2=journal.claim(scope,"owner","bsgs-stop").at(0);
        request=CheckpointRequest::Run;
        b=CheckpointRun::bsgs(journal,bg2,targets,table,verifier,[&](const auto& batch){
            request=CheckpointRequest::Stop;return execute(batch,targets,verifier,1024);
        },options,{}, {},{[&]{return request;},{},{}});
        require(!b.complete && journal.block(scope,UInt256()).covered.empty(),"partial BSGS stop coverage");
        require(journal.results(scope).size()==1,"partial BSGS stop match lost");
        unsigned replayed=0;
        b=CheckpointRun::bsgs(journal,bg2,targets,table,verifier,[&](const auto& batch){
            if(!replayed++)require(batch.first_target()==0,"restart skipped incomplete tile subgroup");
            return execute(batch,targets,verifier,1024);
        },options);
        require(b.complete && journal.results(scope).size()==3,"partial BSGS replay deduplication");

        // An overflow is drained work, but never accepted coverage. Stopping
        // at that boundary must replay every scalar after restart.
        auto xs=x_targets(verifier,{200,201});options.candidate_capacity=1;
        scope=CheckpointRun::create_xpoint(journal,project,ScalarInterval(UInt256(200),UInt256(204)),UInt256(4),xs);
        const auto overflow=journal.claim(scope,"owner","overflow").at(0);request=CheckpointRequest::Run;
        auto over=CheckpointRun::xpoint(journal,overflow,xs,verifier,[&](const auto& batch){
            request=CheckpointRequest::Stop;return execute(batch,xs,verifier,1);
        },options,{}, {},{[&]{return request;},{},{}});
        require(over.overflows==1 && !over.complete && journal.block(scope,UInt256()).covered.empty(),"overflow stop credited coverage");
        over=CheckpointRun::xpoint(journal,overflow,xs,verifier,[&](const auto& batch){return execute(batch,xs,verifier,1);},options);
        require(over.complete && journal.results(scope).size()==2,"overflow restart");
        // A request concurrent with the last batch reports completion, not pause.
        grant=make(300,4);request=CheckpointRequest::Run;bool complete=false;
        const auto final=CheckpointRun::xpoint(journal,grant,absent,verifier,[&](const auto& batch){
            request=CheckpointRequest::Pause;return execute(batch,absent,verifier,1);
        },options,{}, {},{[&]{return request;},[&](CheckpointActivity a){complete=a==CheckpointActivity::Completed;},{}});
        require(final.complete && complete,"last-batch pause hung or reported incomplete");
        journal.check();
        std::cout<<"Pause, stop, resume fences, partial BSGS, overflow and live snapshot checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
