#include "keyhunt/core/xpoint_search.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
int main() {
    try {
        core::XPointVerifier verifier;
        const auto pub = verifier.derive(UInt256(1));
        core::XPointBytes x{}; std::copy_n(pub.begin()+1,32,x.begin());
        core::XPointBytes wrong{};
        core::XPointTargets targets({x,wrong,x});
        require(targets.values().size()==2,"targets not deduplicated");
        require(targets.digest()==core::XPointTargets({wrong,x}).digest(),"target digest depends on input order");
        require(targets.digest()!=core::XPointTargets({x}).digest(),"target digest missed a target");
        rejects([] { core::XPointTargets empty({}); });
        rejects([] { core::XPointTargets invalid({UInt256::from_hex("fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f").bytes()}); });
        rejects([&] { verifier.derive(UInt256(0)); });
        rejects([&] { verifier.derive(core::scalar_order()); });
        scheduler::ExecutionIdentity identity; identity.target_digest=targets.digest();
        identity.assignment_id[0]=1; identity.assignment_generation=identity.executor_generation=1;
        scheduler::BlockGrid grid(core::ScalarInterval(UInt256(1),UInt256(4)),UInt256(3));
        auto work = *scheduler::WorkUnit::plan(grid,UInt256(0),UInt256(1),3,identity);
        auto batch = *scheduler::KernelBatch::plan(work,UInt256(1),3);
        const auto result=verifier.verify(batch,targets,{{0,1,0}});
        require(result.size()==1 && result[0].scalar==UInt256(1),"known match rejected");
        require(verifier.verify(batch,targets,{}).empty(),"empty candidates rejected");
        rejects([&] { verifier.verify(batch,targets,{{3,1,0}}); });
        rejects([&] { verifier.verify(batch,targets,{{0,2,0}}); });
        rejects([&] { verifier.verify(batch,targets,{{0,1,1}}); });
        rejects([&] { verifier.verify(batch,targets,{{0,1,0},{0,1,0}}); });
        rejects([&] { verifier.verify(batch,targets,{{1,1,0}}); });
        rejects([&] { verifier.verify(batch,targets,{{0,0,0}}); });
        rejects([&] { verifier.verify(batch,core::XPointTargets({x}),{}); });
        std::cout << "Xpoint target identity and CPU candidate validation passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
