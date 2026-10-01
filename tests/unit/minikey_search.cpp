#include "keyhunt/core/minikey_search.h"
#include "keyhunt/core/vanity_search.h"
#include <iostream>
#include <stdexcept>
using namespace keyhunt;using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("missing rejection");}
int main(){try{
    core::XPointVerifier verifier;
    const std::string keys[]={"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"};
    const char* scalars[]={"e9873d79c6d87dc0fb6a5778633389f4453213303da61f20bd67fc233aa33262","4c7a9640c72dc2099f23715d0c8a0d8a35f8906e3cab61dd3f78b67bf887c9ab"};
    const char* ordinals[]={"80a4c77e06856b4e84a21a7fe3b746a","5b2532071db1f29d1effc3478ac39b6a3f75b7f145"};
    const char* addresses[]={"1CciesT23BNionJeXrbxmjc7ywfiyM4oLW"};
    for(unsigned k=0;k<2;++k){
        const auto& text=keys[k];const auto ordinal=core::minikey_ordinal(text);const auto length=text.size();
        require(ordinal==UInt256::from_hex(ordinals[k])&&core::minikey_text(ordinal,length)==text,"ordinal fixture differs");
        const auto scalar=core::minikey_scalar(text);require(scalar&&*scalar==UInt256::from_hex(scalars[k]),"published scalar differs");
        const auto pub=verifier.derive(*scalar);
        if(k==1)require(core::bitcoin_address(pub,2)==addresses[0],"published uncompressed address differs");
        const auto end=core::minikey_space_end(length);
        require(core::minikey_text(UInt256(1),length)=="S"+std::string(length-1,'1'),"first ordinal differs");
        require(core::minikey_text(end.subtract(UInt256(1)),length)=="S"+std::string(length-1,'z'),"last ordinal differs");
        for(unsigned power=1;power<length;++power){UInt256 carry(1);for(unsigned p=0;p<power;++p)carry=carry.multiply(UInt256(58));
            for(const auto& at:{carry,carry.add(UInt256(1)),carry.add(UInt256(2))})if(at<end)
                require(core::minikey_ordinal(core::minikey_text(at,length))==at,"base58 carry roundtrip differs");}
        rejects([&]{core::minikey_text(UInt256(),length);});rejects([&]{core::minikey_text(end,length);});
        const auto c=core::minikey_target(length,core::hash160_target(pub,1));const auto u=core::minikey_target(length,core::hash160_target(pub,2));
        const core::MinikeyTargets targets({u,c,u}),same({c,u});require(targets.digest()==same.digest()&&targets.values().size()==2,"canonical identity differs");
        require(targets.max_matches_per_scalar()==2&&targets.encodings()==3,"encoding bound differs");
        auto changed=c;changed[0]=length==22?30:22;require(core::MinikeyTargets({changed}).digest()!=core::MinikeyTargets({c}).digest(),"length not in identity");
        rejects([&]{core::MinikeyTargets bad({c,changed});});changed=c;changed[1]=3;rejects([&]{core::MinikeyTargets bad({changed});});
        targets.validate_interval({UInt256(1),end});rejects([&]{targets.validate_interval({end,end.add(UInt256(1))});});
        scheduler::ExecutionIdentity id;id.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;id.target_digest=targets.digest();
        id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
        scheduler::BlockGrid grid({ordinal,ordinal.add(UInt256(2))},UInt256(2));
        auto plan=[&]{auto work=*scheduler::WorkUnit::plan(grid,UInt256(),ordinal,2,id);return *scheduler::KernelBatch::plan(work,ordinal,2);};
        const auto batch=plan();require(batch.ordinal_at(0)==ordinal,"batch ordinal differs");rejects([&]{batch.scalar_at(0);});rejects([&]{batch.ordinal_at(2);});
        const auto rows=core::verify_minikeys(batch,targets,{{0,1,0},{0,0,0}},verifier);
        require(rows.size()==2&&rows[0].scalar==ordinal,"receipt did not retain ordinal");
        for(auto records:std::vector<std::vector<core::XPointCandidate>>{{{2,0,0}},{{0,2,0}},{{0,0,1}},{{0,0,0},{0,0,0}},{{1,0,0}}})
            rejects([&]{core::verify_minikeys(batch,targets,records,verifier);});
        for(auto mode:{scheduler::WorkAlgorithm::DirectXPointV1,scheduler::WorkAlgorithm::DirectHash160V1,scheduler::WorkAlgorithm::DirectEthereumV1,scheduler::WorkAlgorithm::DirectVanityV1}){
            id.algorithm=mode;rejects([&]{core::verify_minikeys(plan(),targets,{},verifier);});rejects([&]{plan().ordinal_at(0);});}
    }
    for(unsigned length:{0,21,23,26,29,31})rejects([&]{core::minikey_space_end(length);});
    for(auto text:std::vector<std::string>{"",std::string(22,'1'),"S"+std::string(20,'1')+"0","S"+std::string(20,'1')+"O","S"+std::string(20,'1')+"I","S"+std::string(20,'1')+"l","S"+std::string(20,'1')+std::string(1,'\0')})
        rejects([&]{core::minikey_ordinal(text);});
    require(!core::minikey_scalar("S"+std::string(21,'1')),"invalid check byte accepted");
    rejects([]{core::MinikeyTargets empty({});});
    std::cout<<"Minikey 22/30 mapping, public vectors, ordinal boundaries, encoding identity and CPU verification passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
