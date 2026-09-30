#include "keyhunt/core/bsgs_search.h"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace keyhunt;
using core::UInt256;
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn) { try { fn(); } catch(const std::exception&) { return; } throw std::runtime_error("expected rejection"); }
std::string hex(const uint8_t* bytes,size_t n) { const char* d="0123456789abcdef"; std::string s; for(size_t i=0;i<n;++i){s+=d[bytes[i]>>4];s+=d[bytes[i]&15];} return s; }
int main() {
    try {
        core::XPointVerifier verifier;
        const auto g=verifier.derive(UInt256(1)),negative=verifier.derive(core::scalar_order().subtract(UInt256(1)));
        core::BsgsTargets targets({g,negative,g});
        require(targets.values().size()==2,"signs/deduplication");
        require(targets.digest()==core::BsgsTargets({negative,g}).digest(),"unstable digest");
        rejects([]{core::BsgsTargets({});});
        auto bad=g; bad[0]=0; rejects([&]{core::BsgsTargets({bad});});
        bad=g; bad[64]^=1; rejects([&]{core::BsgsTargets({bad});});
        struct File {
            char path[40]="/tmp/keyhunt-bsgs-targets-XXXXXX";
            File(){int fd=mkstemp(path);if(fd<0)throw std::runtime_error("mkstemp");close(fd);}
            ~File(){unlink(path);}
            void write(const std::string& s){std::ofstream f(path,std::ios::binary); f.write(s.data(),s.size());}
        } file;
        const auto compressed=std::string(g[64]&1?"03":"02")+hex(g.data()+1,32);
        for(const auto& ending:{"","\n","\r\n"}) {
            file.write(hex(g.data(),65)+"\n"+compressed+ending);
            require(core::BsgsTargets::load(file.path).values()==core::BsgsTargets({g}).values(),"SEC1 normalization");
        }
        for(const auto& text:{std::string(),std::string(130,'0'),std::string(100000,'0'),compressed+std::string(1,'\0'),
            std::string("02")+std::string(64,'f'),std::string("02")+std::string(64,'0'),std::string("06")+hex(g.data()+1,64)}) {
            file.write(text); rejects([&]{core::BsgsTargets::load(file.path);});
        }
        const auto begin=UInt256::power_of_two(200).subtract(UInt256(3));
        const core::ScalarInterval interval(begin,begin.add(UInt256(18)));
        const auto table=bsgs::Table::build(7);
        core::BsgsTargets known({verifier.derive(begin),verifier.derive(begin.add(UInt256(17)))});
        core::BsgsBatch batch(interval,7,0,2,known.digest(),table.checksum());
        require(batch.giants()==3 && batch.last_babies()==4 && batch.steps()==6,"tail geometry");
        require(batch.scalar_at(2,3)==interval.end().subtract(UInt256(1)),"tail reconstruction");
        rejects([&]{batch.scalar_at(2,4);}); rejects([&]{batch.scalar_at(3,0);}); rejects([&]{batch.scalar_at(0,7);});
        std::vector<core::BsgsCandidate> candidates;
        for(uint32_t t=0;t<2;++t) candidates.push_back(known.values()[t]==verifier.derive(begin)?core::BsgsCandidate{0,0,t,0}:core::BsgsCandidate{2,3,t,0});
        require(core::verify_bsgs(batch,known,verifier,candidates).size()==2,"CPU verification");
        auto corrupt=candidates; corrupt.push_back(candidates[0]); rejects([&]{core::verify_bsgs(batch,known,verifier,corrupt);});
        corrupt=candidates; corrupt[0].reserved=1; rejects([&]{core::verify_bsgs(batch,known,verifier,corrupt);});
        corrupt=candidates; corrupt[0].target=2; rejects([&]{core::verify_bsgs(batch,known,verifier,corrupt);});
        corrupt=candidates; corrupt[0].baby^=1; rejects([&]{core::verify_bsgs(batch,known,verifier,corrupt);});
        rejects([&]{core::verify_bsgs(batch,targets,verifier,{});});
        const uint64_t m=UINT64_MAX;
        const auto wide=core::bsgs_tile(core::ScalarInterval(begin,core::scalar_order()),m,1048576);
        core::BsgsBatch high(wide,m,0,1,known.digest(),{});
        require(high.scalar_at(1048575,m-1)==wide.end().subtract(UInt256(1)),"wide product truncated");
        const auto endtile=core::bsgs_tile(core::ScalarInterval(core::scalar_order().subtract(UInt256(3)),core::scalar_order()),m,1048576);
        require(endtile.size()==UInt256(3),"order endpoint overflow");
        rejects([&]{core::BsgsBatch(interval,0,0,1,{},{});});
        rejects([&]{core::BsgsBatch(interval,1,0,0,{},{});});
        rejects([&]{core::BsgsBatch(interval,1,65535,2,{},{});});
        rejects([&]{core::BsgsBatch(wide,1,0,1,{},{});});
        std::cout<<"BSGS SEC1 identity, wide tile mapping and CPU verification passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n'; return 1;}
}
