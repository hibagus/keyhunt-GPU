#include "keyhunt/core/xpoint_search.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <stdexcept>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn) { try { fn(); } catch (const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
int main() {
    try {
        // A private temporary file exercises the bounded binary-safe parser under
        // ASan/UBSan too; malformed data must never be accepted as a truncated X.
        struct Temporary {
            char path[32] = "/tmp/keyhunt-xpoint-XXXXXX";
            Temporary() { const int fd=mkstemp(path); if (fd<0) throw std::runtime_error("mkstemp failed"); close(fd); }
            ~Temporary() { unlink(path); }
            void write(const std::string& data) { std::ofstream out(path,std::ios::binary); out.write(data.data(),data.size()); }
        } file;
        for (const std::string ending : {"", "\n", "\r\n"}) {
            file.write(std::string(64,'0')+ending);
            require(core::XPointTargets::load(file.path).values().size()==1,"valid target file rejected");
        }
        for (const auto& data : {std::string(),std::string(63,'0'),std::string(65,'0'),std::string(64,'g'),
            std::string(64,'0')+std::string(1,'\0')+"\n",std::string(100000,'0')}) {
            file.write(data); rejects([&] { core::XPointTargets::load(file.path); });
        }
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
        identity.algorithm=scheduler::WorkAlgorithm::DirectHash160V1;
        const auto mislabeled=*scheduler::WorkUnit::plan(grid,UInt256(0),UInt256(1),3,identity);
        rejects([&]{verifier.verify(*scheduler::KernelBatch::plan(mislabeled,UInt256(1),3),targets,{});});
        std::cout << "Xpoint target identity and CPU candidate validation passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
