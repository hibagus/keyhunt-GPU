#include "keyhunt/core/ethereum_search.h"
#include "keyhunt/core/hash160_search.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
struct Temporary {
    char path[40]="/tmp/keyhunt-ethereum-XXXXXX";
    Temporary(){const int fd=mkstemp(path);if(fd<0)throw std::runtime_error("mkstemp failed");close(fd);}
    ~Temporary(){unlink(path);}
    void write(const std::string& text){std::ofstream out(path,std::ios::binary);out.write(text.data(),text.size());}
};
int main(){try{
    const std::string address="7e5f4552091a69125d5dfcb7b8c2659029395bdf";
    const auto target=core::parse_ethereum_address(address);
    core::XPointVerifier verifier;
    require(core::ethereum_target(verifier.derive(UInt256(1)))==target,"public scalar-1 Ethereum vector differs");
    auto pub=verifier.derive(UInt256(1));pub[0]=2;
    rejects([&]{core::ethereum_target(pub);});
    // Published ERC-55 vectors cover upper, lower and mixed-case checksums.
    for(const std::string text:{"52908400098527886E0F7030069857D2E4169EE7","8617E340B3D01FA5F11F306F4090FD50E238070D",
        "de709f2102306220921060314715629080e2fb77","27b1fdb04752bbc536007a920d24acb045561c26",
        "5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed","fB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
        "dbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB","D1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb"}) {
        auto lower=text;std::transform(lower.begin(),lower.end(),lower.begin(),[](char c){return c>='A'&&c<='F'?char(c-'A'+'a'):c;});
        require(core::parse_ethereum_address("0x"+text)==core::parse_ethereum_address(lower),"ERC-55 vector rejected");
    }
    Temporary file;file.write(address);
    const auto targets=core::EthereumTargets::load(file.path);
    for(const std::string text:std::vector<std::string>{address,"0x"+address,"0x7E5F4552091A69125D5DFCB7B8C2659029395BDF",
                               "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf"}) {
        file.write(text+"\r\n\n"+address);
        require(core::EthereumTargets::load(file.path).digest()==targets.digest(),"equivalent encoding changes job digest");
    }
    for(const auto& text:{std::string(),std::string(39,'0'),std::string(41,'0'),std::string(40,'z'),
        address+std::string(1,'\0'),"0X"+address," "+address,std::string(100000,'a'),
        std::string("0x7e5F4552091A69125d5DfCb7b8C2659029395Bdf")}) {
        file.write(text);rejects([&]{core::EthereumTargets::load(file.path);});
    }
    rejects([]{core::EthereumTargets empty({});});
    const auto second=core::ethereum_target(verifier.derive(UInt256(2)));
    require(core::EthereumTargets({target,second,target}).digest()==core::EthereumTargets({second,target}).digest(),"ordering/dedup changes identity");
    core::Hash160Target bitcoin{};bitcoin[0]=1;std::copy(target.begin(),target.end(),bitcoin.begin()+1);
    require(core::Hash160Targets({bitcoin}).digest()!=targets.digest(),"cross-family digest collision");
    scheduler::ExecutionIdentity identity;identity.target_digest=targets.digest();
    identity.algorithm=scheduler::WorkAlgorithm::DirectEthereumV1;
    identity.assignment_id[0]=1;identity.assignment_generation=identity.executor_generation=1;
    scheduler::BlockGrid grid({UInt256(1),UInt256(4)},UInt256(3));
    auto batch_for=[&](const auto& id){const auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),UInt256(1),3,id);
        return *scheduler::KernelBatch::plan(work,UInt256(1),3);};
    const auto batch=batch_for(identity);
    const auto found=core::verify_ethereum(batch,targets,{{0,0,0}},verifier);
    require(found.size()==1&&found[0].scalar==UInt256(1),"CPU candidate verification failed");
    for(const auto& candidates:std::vector<std::vector<core::XPointCandidate>>{
        {{3,0,0}},{{0,1,0}},{{0,0,1}},{{0,0,0},{0,0,0}},{{1,0,0}}})
        rejects([&]{core::verify_ethereum(batch,targets,candidates,verifier);});
    for(const auto algorithm:{scheduler::WorkAlgorithm::DirectXPointV1,scheduler::WorkAlgorithm::DirectHash160V1}) {
        identity.algorithm=algorithm;
        rejects([&]{core::verify_ethereum(batch_for(identity),targets,{},verifier);});
    }
    identity.algorithm=scheduler::WorkAlgorithm::DirectEthereumV1;identity.target_digest[0]^=1;
    rejects([&]{core::verify_ethereum(batch_for(identity),targets,{},verifier);});
    std::cout<<"Ethereum canonical inputs, ERC-55, CPU derivation and candidate identity passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
