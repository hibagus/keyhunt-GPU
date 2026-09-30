#include "checkpoint_fixture.h"
#include "sqlite.h"
#include <iostream>
#include <sys/wait.h>
using namespace fixture;
using namespace keyhunt::storage::detail;
int main(){
    try{
        Temporary temporary;core::XPointVerifier verifier;
        const auto xs=x_targets(verifier,{1,3,5});const auto bs=b_targets(verifier,{1,3,5});const auto table=bsgs::Table::build(3);
        CheckpointOptions options;options.xpoint_steps=5;options.giant_steps=2;options.target_batch=3;options.candidate_capacity=8;options.checkpoint_seconds=0;
        unsigned cases=0;
        for(bool bsgs_mode:{false,true})for(const std::string fault:{"after_execute","after_verification","after_results","after_coverage","before_commit","after_commit","after_ack"}){
            const auto directory=(temporary.path/(std::to_string(bsgs_mode)+"-"+fault)).string();
            // No SQLite connection is inherited across fork.
            auto setup=[&]{
                Journal j(directory);const auto project=j.create_project("fault boundary");
                const auto root=ScalarInterval(UInt256(1),UInt256(6));
                const auto scope=bsgs_mode?CheckpointRun::create_bsgs(j,project,root,UInt256(5),bs,table):
                    CheckpointRun::create_xpoint(j,project,root,UInt256(5),xs);
                return j.claim(scope,"worker","claim").at(0);
            };
            const auto grant=setup();
            const auto child=fork();require(child>=0,"fork");
            if(!child){
                try{
                    Journal j(directory);
                    auto arm=[&]{
                        if(fault=="after_execute")_exit(73);
                        transaction_test_hook=[&](const char* boundary){if(fault==boundary)_exit(73);};
                    };
                    const auto observer=[&](const auto&,size_t,double){if(fault=="after_ack")_exit(73);};
                    if(bsgs_mode)CheckpointRun::bsgs(j,grant,bs,table,verifier,[&](const auto& batch){
                        auto result=execute(batch,bs,verifier,8);arm();return result;
                    },options,observer);
                    else CheckpointRun::xpoint(j,grant,xs,verifier,[&](const auto& batch){
                        auto result=execute(batch,xs,verifier,8);arm();return result;
                    },options,observer);
                }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(74);}
                _exit(75);
            }
            int status=0;require(waitpid(child,&status,0)==child,"waitpid");
            require(WIFEXITED(status)&&WEXITSTATUS(status)==73,"fault hook not reached");
            Journal j(directory);j.check();const bool committed=fault=="after_commit"||fault=="after_ack";
            require(j.results(grant.scope).size()==(committed?3:0),"atomic result visibility");
            require((j.block(grant.scope,UInt256()).state=="finished")==committed,"atomic coverage visibility");
            unsigned replayed=0;
            if(bsgs_mode)CheckpointRun::bsgs(j,grant,bs,table,verifier,[&](const auto& b){++replayed;return execute(b,bs,verifier,8);},options);
            else CheckpointRun::xpoint(j,grant,xs,verifier,[&](const auto& b){++replayed;return execute(b,xs,verifier,8);},options);
            require(replayed==(committed?0:1),"incorrect replay complement");
            require(j.results(grant.scope).size()==3&&j.block(grant.scope,UInt256()).state=="finished","restart lost matches/coverage");
            j.check();++cases;
        }
        std::cout<<cases<<" real process exits across both modes and seven execution/commit/ack boundaries passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
