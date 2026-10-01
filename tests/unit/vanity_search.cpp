#include "keyhunt/core/vanity_search.h"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
struct Temporary {
    char path[40]="/tmp/keyhunt-vanity-XXXXXX";
    Temporary(){const int fd=mkstemp(path);if(fd<0)throw std::runtime_error("mkstemp failed");close(fd);}
    ~Temporary(){unlink(path);}
    void write(const std::string& text){std::ofstream out(path,std::ios::binary);out.write(text.data(),text.size());}
};
int main(){try{
    core::XPointVerifier verifier;const auto pub=verifier.derive(UInt256(1));
    require(core::bitcoin_address(pub,1)=="1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH","compressed address differs");
    require(core::bitcoin_address(pub,2)=="1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm","uncompressed address differs");
    Temporary file;file.write("1\n1B\n1Bg\n");const auto targets=core::VanityTargets::load(file.path);
    require(targets.values().size()==6&&targets.max_matches_per_scalar()==6,"overlapping prefix bound differs");
    file.write("1Bg\r\n1B\n1\n1B\n\n");require(core::VanityTargets::load(file.path).digest()==targets.digest(),"ordering/dedup changed identity");
    file.write("1b");const auto lower=core::VanityTargets::load(file.path);file.write("1B");
    require(core::VanityTargets::load(file.path).digest()!=lower.digest(),"case folded prefix");
    require(core::VanityTargets::load(file.path,core::Hash160Encoding::Compressed).max_matches_per_scalar()==1,"encoding bound differs");
    for(const auto& invalid:std::vector<std::string>{""," ","0x123","3ABC","bc1q","1O","1I","1l","10",std::string(35,'1'),std::string("1\0B",3),"1 B",std::string(100000,'1')}) {
        file.write(invalid);rejects([&]{core::VanityTargets::load(file.path);});
    }
    rejects([]{core::VanityTargets none({});});
    auto invalid=core::vanity_target("1B",1);invalid[35]=1;rejects([&]{core::VanityTargets bad({invalid});});
    invalid[35]=0;invalid[0]=3;rejects([&]{core::VanityTargets bad({invalid});});
    invalid[0]=1;invalid[1]=255;rejects([&]{core::VanityTargets bad({invalid});});
    std::vector<core::VanityTarget> chain;
    for(uint8_t tag:{1,2})for(unsigned length=1;length<=34;++length)chain.push_back(core::vanity_target(std::string(length,'1'),tag));
    require(core::VanityTargets(chain).max_matches_per_scalar()==68,"maximum prefix-length bound incorrect");
    chain.resize(4097,core::vanity_target("1",1));rejects([&]{core::VanityTargets bad(chain);});
    scheduler::ExecutionIdentity id;id.algorithm=scheduler::WorkAlgorithm::DirectVanityV1;id.target_digest=targets.digest();
    id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    scheduler::BlockGrid grid({UInt256(1),UInt256(4)},UInt256(3));
    auto batch_for=[&]{auto work=*scheduler::WorkUnit::plan(grid,UInt256(),UInt256(1),3,id);return *scheduler::KernelBatch::plan(work,UInt256(1),3);};
    auto batch=batch_for();
    auto found=core::verify_vanity(batch,targets,{{0,2,0},{0,0,0},{0,1,0},{0,3,0}},verifier);
    require(found.size()==4,"overlapping prefixes or both encodings lost");
    for(auto records:std::vector<std::vector<core::XPointCandidate>>{{{3,0,0}},{{0,6,0}},{{0,0,1}},{{0,0,0},{0,0,0}},{{0,4,0}}})
        rejects([&]{core::verify_vanity(batch,targets,records,verifier);});
    for(auto mode:{scheduler::WorkAlgorithm::DirectXPointV1,scheduler::WorkAlgorithm::DirectHash160V1,scheduler::WorkAlgorithm::DirectEthereumV1}){
        id.algorithm=mode;rejects([&]{core::verify_vanity(batch_for(),targets,{},verifier);});}
    std::cout<<"Vanity canonical prefixes, length bounds, overlapping relations and CPU verification passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
