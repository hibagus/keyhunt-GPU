#include "keyhunt/core/hash160_search.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace keyhunt;
using core::UInt256;
using core::Hash160Targets;
using core::Hash160Encoding;
using core::Hash160Input;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
struct Temporary {
    char path[40]="/tmp/keyhunt-hash160-XXXXXX";
    Temporary(){const int fd=mkstemp(path);if(fd<0)throw std::runtime_error("mkstemp failed");close(fd);}
    ~Temporary(){unlink(path);}
    void write(const std::string& text){std::ofstream out(path,std::ios::binary);out.write(text.data(),text.size());}
};
int main(){try{
    // Public scalar-1 P2PKH vectors cover both SEC1 serializations. Address and
    // raw input parsing must produce the same tagged relation and job digest.
    Temporary file;const std::string compressed="751e76e8199196d454941c45d1b3a323f1433bd6";
    const std::string uncompressed="91b24bf9f5288532960ac687abb035127b1d28a5";
    file.write(compressed);const auto c=Hash160Targets::load(file.path,Hash160Input::Hex,Hash160Encoding::Compressed);
    file.write("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");
    require(Hash160Targets::load(file.path,Hash160Input::BitcoinAddress,Hash160Encoding::Compressed).digest()==c.digest(),"address/raw identity mismatch");
    file.write(uncompressed);const auto u=Hash160Targets::load(file.path,Hash160Input::Hex,Hash160Encoding::Uncompressed);
    file.write("1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm");
    require(Hash160Targets::load(file.path,Hash160Input::BitcoinAddress,Hash160Encoding::Uncompressed).digest()==u.digest(),"uncompressed address mismatch");
    core::XPointVerifier verifier;const auto pub=verifier.derive(UInt256(1));
    require(core::hash160_target(pub,1)==c.values()[0]&&core::hash160_target(pub,2)==u.values()[0],"CPU HASH160 public vectors differ");
    for(const std::string ending:{"","\n","\r\n"}){
        file.write(compressed+ending);require(Hash160Targets::load(file.path,Hash160Input::Hex).values().size()==2,"both encoding load failed");
    }
    file.write(compressed+"\n"+compressed+"\r\n\n");
    const auto both=Hash160Targets::load(file.path,Hash160Input::Hex);
    require(both.values().size()==2&&both.encodings()==3&&both.max_matches_per_scalar()==2,"tagged deduplication failed");
    require(both.digest()!=c.digest(),"compression omitted from digest");
    auto upper=compressed;std::transform(upper.begin(),upper.end(),upper.begin(),::toupper);
    file.write(upper);require(Hash160Targets::load(file.path,Hash160Input::Hex).digest()==both.digest(),"hex case changed identity");
    for(const auto& invalid:{std::string(),std::string(39,'0'),std::string(41,'0'),std::string(40,'z'),
                            compressed+std::string(1,'\0'),std::string(100000,'a')}){
        file.write(invalid);rejects([&]{Hash160Targets::load(file.path,Hash160Input::Hex);});
    }
    for(const auto& invalid:{"1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMJ","11BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH",
                            "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy","mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJRfn","0OIl"}){
        file.write(invalid);rejects([&]{Hash160Targets::load(file.path,Hash160Input::BitcoinAddress);});
    }
    rejects([]{Hash160Targets targets({});});
    auto invalid=c.values()[0];invalid[0]=3;rejects([&]{Hash160Targets targets({invalid});});
    rejects([]{core::hash160_encoding("all");});
    Hash160Targets targets({u.values()[0],c.values()[0],c.values()[0]});
    require(targets.values().size()==2&&targets.digest()==Hash160Targets({c.values()[0],u.values()[0]}).digest(),"order changed target digest");
    scheduler::ExecutionIdentity identity;identity.target_digest=targets.digest();
    identity.algorithm=scheduler::WorkAlgorithm::DirectHash160V1;
    identity.assignment_id[0]=1;identity.assignment_generation=identity.executor_generation=1;
    scheduler::BlockGrid grid(core::ScalarInterval(UInt256(1),UInt256(4)),UInt256(3));
    const auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),UInt256(1),3,identity);
    const auto batch=*scheduler::KernelBatch::plan(work,UInt256(1),3);
    auto verify=[&](std::vector<core::XPointCandidate> candidates){return core::verify_hash160(batch,targets,std::move(candidates),verifier);};
    const auto found=verify({{0,1,0},{0,0,0}});
    require(found.size()==2&&found[0].scalar==UInt256(1)&&found[1].scalar==UInt256(1),"two valid encodings at one scalar rejected");
    require(verify({}).empty(),"empty candidate set failed");
    for(const auto& candidates:std::vector<std::vector<core::XPointCandidate>>{
        {{3,0,0}},{{0,2,0}},{{0,0,1}},{{0,0,0},{0,0,0}},{{1,0,0}}})rejects([&]{verify(candidates);});
    rejects([&]{core::verify_hash160(batch,c,{},verifier);});
    identity.algorithm=scheduler::WorkAlgorithm::DirectXPointV1;
    const auto wrong=*scheduler::WorkUnit::plan(grid,UInt256(0),UInt256(1),3,identity);
    rejects([&]{core::verify_hash160(*scheduler::KernelBatch::plan(wrong,UInt256(1),3),targets,{},verifier);});
    std::cout<<"Canonical P2PKH/HASH160 inputs, encoding identity and CPU candidate verification passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
