#include "keyhunt/backend/hip_xpoint.h"
#include "keyhunt/backend/device.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
scheduler::KernelBatch plan(UInt256 begin, uint64_t count, const core::XPointTargets& targets) {
    scheduler::ExecutionIdentity identity;
    identity.target_digest=targets.digest(); identity.job_digest[0]=11;
    identity.assignment_id[5]=44; identity.assignment_generation=5; identity.executor_generation=6;
    scheduler::BlockGrid grid(core::ScalarInterval(begin.subtract(UInt256(count+2)),begin.add(UInt256(count))),UInt256(count+1));
    auto work=*scheduler::WorkUnit::plan(grid,UInt256(1),begin,count,identity);
    return *scheduler::KernelBatch::plan(work,begin,count);
}
int main() {
    try {
        require(!backend::discover_hip().devices.empty(),"hardware required");
        core::XPointVerifier verifier;
        auto begin=UInt256::from_hex("100000000fffffffffffffffe");
        std::vector<core::XPointBytes> values;
        for (unsigned i=0;i<1025;++i) {
            const auto pub=verifier.derive(begin.add(UInt256(i)));
            core::XPointBytes x{}; std::copy_n(pub.begin()+1,32,x.begin()); values.push_back(x);
        }
        core::XPointTargets targets(values);
        backend::XPointOptions options; options.max_steps=1025; options.candidate_capacity=1025;
        rejects([&] { backend::HipXPointExecutor e(-1,targets,verifier,options); });
        rejects([&] { auto bad=options; bad.max_steps=0; backend::HipXPointExecutor e(0,targets,verifier,bad); });
        rejects([&] { auto bad=options; bad.max_steps=1048577; backend::HipXPointExecutor e(0,targets,verifier,bad); });
        rejects([&] { auto bad=options; bad.candidate_capacity=0; backend::HipXPointExecutor e(0,targets,verifier,bad); });
        rejects([&] { auto bad=options; bad.memory_reserve_bytes=std::numeric_limits<uint64_t>::max(); backend::HipXPointExecutor e(0,targets,verifier,bad); });
        backend::HipXPointExecutor e(0,targets,verifier,options), other(0,targets,verifier,options);
        auto first=plan(begin,257,targets);
        auto ticket=e.submit(first);
        rejects([&] { e.submit(first); });
        auto other_ticket=other.submit(first);
        rejects([&] { other.poll(ticket); });
        other.drain(); require(other.take(other_ticket).matches.size()==257,"independent executor failed");
        e.drain(); rejects([&] { e.submit(first); });
        const auto kept=e.take(ticket);
        require(kept.matches.size()==257 && kept.verified_steps==257,"dense batch incomplete");
        require(kept.batch.work().identity()==first.work().identity() && kept.batch.work().block_id()==UInt256(1),"identity lost");
        rejects([&] { e.take(ticket); });
        for (unsigned count : {1,2,3,4,5,7,8,9,31,32,33,127,128,129,255,256,257,1023,1024,1025}) {
            const auto next=e.submit(plan(begin,count,targets));
            rejects([&] { e.poll(ticket); });
            e.drain(); const auto r=e.take(next);
            require(!r.overflow && r.device_steps==count && r.verified_steps==count && r.matches.size()==count,"tail coverage failure");
            for (unsigned i=0;i<count;++i) require(r.matches[i].scalar==begin.add(UInt256(i)),"gap or duplicate scalar");
        }
        auto small_options=options; small_options.candidate_capacity=1;
        backend::HipXPointExecutor small(0,targets,verifier,small_options);
        auto overflow=small.submit(first); small.drain(); const auto r=small.take(overflow);
        require(r.overflow && r.candidate_count==257 && r.matches.empty() && !r.verified_steps,"overflow credited coverage");
        // The slot remains usable after overflow; replay never consumes its prefix.
        auto replay=small.submit(plan(begin,1,targets)); small.drain();
        require(small.take(replay).matches.size()==1,"replay failed");
        require(kept.matches.front().scalar==begin && kept.matches.back().scalar==begin.add(UInt256(256)),"owned result mutated");
        rejects([&] { e.submit(plan(begin,1026,targets)); });
        rejects([&] { e.submit(plan(begin,1,core::XPointTargets({core::XPointBytes{}}))); });
        backend::HipDiagnosticExecutor diagnostic(0);
        auto dt=diagnostic.submit(first);
        auto xt=e.submit(first);
        rejects([&] { e.poll(dt); }); rejects([&] { diagnostic.poll(xt); });
        diagnostic.drain(); (void)diagnostic.take(dt); e.drain(); (void)e.take(xt);
        { backend::HipXPointExecutor pending(0,targets,verifier,options); pending.submit(first); }
        std::cout << "HIP xpoint ownership, every-index tails, overflow and replay passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
