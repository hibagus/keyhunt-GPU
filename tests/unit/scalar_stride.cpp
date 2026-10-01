#include "keyhunt/scheduler/work_unit.h"
#include "keyhunt/core/hash160_search.h"
#include "keyhunt/core/ethereum_search.h"
#include "keyhunt/core/vanity_search.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace keyhunt;using namespace core;using namespace scheduler;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
ExecutionIdentity identity(WorkAlgorithm algorithm){
    ExecutionIdentity id;id.algorithm=algorithm;id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;return id;
}
int main(){try{
    for(unsigned start=1;start<=3;++start)for(unsigned span=1;span<=17;++span)for(unsigned stride=2;stride<=19;++stride){
        const ScalarStride mapping({UInt256(start),UInt256(start+span)},UInt256(stride));
        require(mapping.indices().size()==UInt256((span-1)/stride+1),"candidate count differs");
        for(unsigned width:{1,3,8}){
            BlockGrid grid(mapping.indices(),UInt256(width));auto id=identity(WorkAlgorithm::StridedXPointV1);id.stride_mapping=mapping;
            uint64_t count=0;
            for(uint64_t block=0;block<grid.count().to_uint64();++block){auto cursor=grid.block(UInt256(block)).begin();
                while(auto work=WorkUnit::plan(grid,UInt256(block),cursor,5,id)){
                    while(auto batch=KernelBatch::plan(*work,cursor,2)){
                        for(uint64_t i=0;i<batch->step_count();++i){
                            require(batch->coordinate_at(i)==UInt256(count+1),"coverage index gap or overlap");
                            const auto scalar=UInt256(start+count*stride);
                            require(batch->scalar_at(i)==scalar&&mapping.index(scalar)==UInt256(count+1),"scalar mapping differs");++count;
                        }
                        cursor=batch->interval().end();
                    }
                }
            }
            require(count==mapping.indices().size().to_uint64(),"incomplete progression");
        }
        rejects([&]{mapping.scalar(UInt256());});rejects([&]{mapping.scalar(mapping.indices().end());});
        if(span>1)rejects([&]{mapping.index(UInt256(start+1));});
    }
    const auto n=scalar_order();
    rejects([&]{ScalarStride({UInt256(1),n},UInt256());});
    rejects([&]{ScalarStride({UInt256(1),n},UInt256(1));});
    rejects([&]{ScalarStride({UInt256(1),n},n);});
    for(unsigned bit=0;bit<256;++bit){
        require(scalar_stride_power(n.subtract(UInt256(1)),bit)==n.subtract(UInt256::power_of_two(bit)),"wide modular doubling differs");
    }
    rejects([&]{scalar_stride_power(UInt256(2),256);});
    const ScalarStride wide({n.subtract(UInt256(9)),n},UInt256(4));
    require(wide.scalar(UInt256(3))==n.subtract(UInt256(1)),"order tail lost");
    // The same receipt index must be verified against its mapped private scalar,
    // independently for every family and every enabled encoding relation.
    XPointVerifier verifier;const ScalarStride mapping({UInt256(100),UInt256(120)},UInt256(7));
    const auto pub=verifier.derive(UInt256(107));XPointBytes x{};std::copy_n(pub.begin()+1,32,x.begin());
    const XPointTargets xt({x});const Hash160Targets ht({hash160_target(pub,1),hash160_target(pub,2)});
    const EthereumTargets et({ethereum_target(pub)});
    const VanityTargets vt({vanity_target(bitcoin_address(pub,1),1),vanity_target(bitcoin_address(pub,2),2)});
    for(auto family:{WorkAlgorithm::DirectXPointV1,WorkAlgorithm::DirectHash160V1,WorkAlgorithm::DirectEthereumV1,WorkAlgorithm::DirectVanityV1}){
        auto id=identity(strided_algorithm(family));id.stride_mapping=mapping;
        id.target_digest=family==WorkAlgorithm::DirectXPointV1?xt.digest():family==WorkAlgorithm::DirectHash160V1?ht.digest():family==WorkAlgorithm::DirectEthereumV1?et.digest():vt.digest();
        const BlockGrid grid(mapping.indices(),UInt256(3));auto work=*WorkUnit::plan(grid,UInt256(),UInt256(1),3,id);
        const auto batch=*KernelBatch::plan(work,UInt256(1),3);
        const auto check=[&](std::vector<XPointCandidate> candidates){
            if(family==WorkAlgorithm::DirectXPointV1)return verifier.verify(batch,xt,candidates);
            if(family==WorkAlgorithm::DirectHash160V1)return verify_hash160(batch,ht,candidates,verifier);
            if(family==WorkAlgorithm::DirectEthereumV1)return verify_ethereum(batch,et,candidates,verifier);
            return verify_vanity(batch,vt,candidates,verifier);
        };
        const auto matches=check({{1,0,0}});require(matches.size()==1&&matches[0].scalar==UInt256(2),"receipt stored private scalar instead of index");
        rejects([&]{check({{0,0,0}});});rejects([&]{check({{1,0,0},{1,0,0}});});rejects([&]{batch.scalar_at(3);});rejects([&]{batch.ordinal_at(0);});
        auto changed=id;changed.stride_mapping=ScalarStride({UInt256(100),UInt256(120)},UInt256(8));require(changed!=id,"stride identity ignored");
        changed=id;changed.stride_mapping.reset();rejects([&]{WorkUnit::plan(grid,UInt256(),UInt256(1),3,changed);});
        changed=id;changed.algorithm=family;rejects([&]{WorkUnit::plan(grid,UInt256(),UInt256(1),3,changed);});
        rejects([&]{WorkUnit::plan(BlockGrid({UInt256(1),UInt256(5)},UInt256(4)),UInt256(),UInt256(1),3,id);});
    }
    rejects([&]{strided_algorithm(WorkAlgorithm::DirectMinikeysV1);});
    std::cout<<"Exact positive stride planning, immutable identity and four-family receipt verification passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
