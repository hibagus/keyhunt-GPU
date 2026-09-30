#include "checkpoint_fixture.h"
#include <csignal>
#include <iostream>
#include <sys/wait.h>
using namespace fixture;
int main(){
    try{
        Temporary temporary;Journal journal(temporary.path.string());core::XPointVerifier verifier;
        const auto targets=x_targets(verifier,{1,8,9,16});
        const auto project=journal.create_project("concurrent");
        const auto scope=CheckpointRun::create_xpoint(journal,project,ScalarInterval(UInt256(1),UInt256(17)),UInt256(8),targets);
        Selection selection;selection.count=2;
        const auto grants=journal.claim(scope,"machine","claim",selection,3600);
        CheckpointOptions options;options.concurrent_blocks=true;options.xpoint_steps=4;
        int ready[2];require(pipe(ready)==0,"pipe");
        const pid_t child=fork();require(child>=0,"fork");
        if(child==0){
            close(ready[0]);
            try{
                Journal separate(temporary.path.string());
                CheckpointRun::xpoint(separate,grants[0],targets,verifier,[&](const auto& batch){
                    const char byte='r';if(write(ready[1],&byte,1)!=1)_exit(3);
                    // Simulate a GPU stuck inside submission, before any receipt.
                    for(;;)pause();
                    return execute(batch,targets,verifier,1024);
                },options);
            }catch(...){_exit(4);}
            _exit(5);
        }
        struct Child {pid_t pid;~Child(){if(pid>0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}}} owner{child};
        close(ready[1]);char byte=0;require(read(ready[0],&byte,1)==1,"child owns block");close(ready[0]);
        const auto run=[&](const Grant& grant,CheckpointOptions limits){
            return CheckpointRun::xpoint(journal,grant,targets,verifier,[&](const auto& batch){
                return execute(batch,targets,verifier,1024);
            },limits);
        };
        rejects([&]{run(grants[0],options);}); // A second descriptor cannot share this block.
        rejects([&]{run(grants[1],CheckpointOptions{});}); // Standalone remains exclusive.
        require(run(grants[1],options).complete,"healthy block completes beside hung owner");
        kill(child,SIGKILL);int status=0;require(waitpid(child,&status,0)==child,"reap old executor");owner.pid=-1;
        require(run(grants[0],options).complete,"crashed block replays after lock release");
        require(journal.results(scope,0,100).size()==4,"exact verified boundary matches");
        journal.check();std::cout<<"concurrent checkpoint ownership passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
