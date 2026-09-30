// CPU-only process fixture: real owner, journal, socket and signal code, with a
// deliberately slow bounded CPU runner. No test switch enters the production CLI.
#include "checkpoint_fixture.h"
#include "checkpoint_control.h"
#include "state_helpers.h"
#include <chrono>
#include <iostream>
#include <thread>
using namespace fixture;
int main(int argc,char** argv){
    try{
        require(argc==3,"driver requires directory and delay in ms");
        const unsigned delay=std::stoul(argv[2]);
        Journal journal(argv[1]);core::XPointVerifier verifier;
        const auto targets=x_targets(verifier,{100000});
        const auto project=journal.create_project("C14 process fixture");
        const auto scope=CheckpointRun::create_xpoint(journal,project,
            ScalarInterval(UInt256(1),UInt256(65537)),UInt256(65536),targets);
        const auto grant=journal.claim(scope,"fixture","claim").at(0);
        std::cout<<"{\"type\":\"fixture\",\"project\":\""<<project<<"\",\"job\":\""
            <<backend::state_detail::hex(scope.job.data(),32)<<"\"}\n"<<std::flush;
        backend::LocalCheckpointControl control(journal.state_directory(),grant,0,1);
        CheckpointOptions options;options.xpoint_steps=4;options.checkpoint_seconds=60;
        unsigned launches=0;
        const auto result=CheckpointRun::xpoint(journal,grant,targets,verifier,[&](const auto& batch){
            if(!launches++)std::cout<<"{\"type\":\"submitted\"}\n"<<std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            return execute(batch,targets,verifier,1024);
        },options,{},[&]{control.close();},control.callbacks());
        std::cout<<"{\"type\":\"summary\",\"complete\":"<<(result.complete?"true":"false")
            <<",\"launches\":"<<launches<<",\"checkpoints\":"<<result.checkpoints<<"}\n"<<std::flush;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
