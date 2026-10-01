#include "keyhunt/scheduler/work_unit.h"
#include "keyhunt/core/hash160_search.h"
#include "keyhunt/core/ethereum_search.h"
#include "keyhunt/core/vanity_search.h"
#include <algorithm>
#include <iostream>
using namespace keyhunt;using namespace core;using namespace scheduler;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
ExecutionIdentity identity(WorkAlgorithm family,const ScalarStride& mapping,const Digest& targets={}){
    ExecutionIdentity id;id.algorithm=strided_algorithm(family,true);id.stride_mapping=mapping;
    id.target_digest=targets;id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;return id;
}
int main(){try{
    for(unsigned begin:{1,7,31})for(unsigned span=1;span<=33;++span)for(unsigned step=1;step<=19;++step){
        const ScalarStride mapping({UInt256(begin),UInt256(begin+span)},UInt256(step),true);
        const unsigned count=1+(span-1)/step;
        require(mapping.indices().size()==UInt256(count),"reverse count differs");
        for(unsigned width:{1,5,17}){
            const BlockGrid grid(mapping.indices(),UInt256(width));const auto id=identity(WorkAlgorithm::DirectXPointV1,mapping);
            unsigned seen=0;
            for(uint64_t block=0;block<grid.count().to_uint64();++block){auto cursor=grid.block(UInt256(block)).begin();
                while(auto work=WorkUnit::plan(grid,UInt256(block),cursor,7,id)){
                    while(auto batch=KernelBatch::plan(*work,cursor,3)){
                        for(uint64_t i=0;i<batch->step_count();++i){
                            const UInt256 scalar(begin+(count-1-seen)*step);
                            require(batch->scalar_reverse()&&batch->scalar_at(i)==scalar&&batch->coordinate_at(i)==UInt256(seen+1),"reverse coverage order differs");
                            require(mapping.index(scalar)==UInt256(seen+1),"reverse inverse differs");++seen;
                        }
                        cursor=batch->interval().end();
                    }
                }
            }
            require(seen==count,"reverse candidate count incomplete");
        }
        rejects([&]{mapping.scalar(UInt256());});rejects([&]{mapping.scalar(UInt256(count+1));});
        if(step>1 && span>1)rejects([&]{mapping.index(UInt256(begin+1));});
    }
    const ScalarStride full({UInt256(1),scalar_order()},UInt256(1),true);
    require(full.scalar(UInt256(1))==scalar_order().subtract(UInt256(1))&&full.scalar(full.indices().size())==UInt256(1),"full-domain endpoints differ");
    rejects([&]{ScalarStride({UInt256(1),UInt256(2)},UInt256(),true);});
    rejects([&]{ScalarStride({UInt256(1),UInt256(2)},scalar_order(),true);});
    XPointVerifier verifier;const auto pub=verifier.derive(UInt256(115));XPointBytes x{};std::copy_n(pub.begin()+1,32,x.begin());
    const XPointTargets xt({x});const Hash160Targets ht({hash160_target(pub,1),hash160_target(pub,2)});
    const EthereumTargets et({ethereum_target(pub)});const VanityTargets vt({vanity_target(bitcoin_address(pub,1),1)});
    const ScalarStride mapping({UInt256(101),UInt256(123)},UInt256(7),true); // 122,115,108,101
    for(auto family:{WorkAlgorithm::DirectXPointV1,WorkAlgorithm::DirectHash160V1,WorkAlgorithm::DirectEthereumV1,WorkAlgorithm::DirectVanityV1}){
        const auto digest=family==WorkAlgorithm::DirectXPointV1?xt.digest():family==WorkAlgorithm::DirectHash160V1?ht.digest():family==WorkAlgorithm::DirectEthereumV1?et.digest():vt.digest();
        const auto id=identity(family,mapping,digest);const BlockGrid grid(mapping.indices(),UInt256(4));
        const auto work=*WorkUnit::plan(grid,UInt256(),UInt256(1),4,id);const auto batch=*KernelBatch::plan(work,UInt256(1),4);
        const auto check=[&](uint64_t at){const std::vector<XPointCandidate> candidates{{at,0,0}};
            if(family==WorkAlgorithm::DirectXPointV1)return verifier.verify(batch,xt,candidates);
            if(family==WorkAlgorithm::DirectHash160V1)return verify_hash160(batch,ht,candidates,verifier);
            if(family==WorkAlgorithm::DirectEthereumV1)return verify_ethereum(batch,et,candidates,verifier);
            return verify_vanity(batch,vt,candidates,verifier);
        };
        require(check(1).at(0).scalar==UInt256(2),"reverse receipt lost candidate index");rejects([&]{check(2);});
        auto changed=id;changed.stride_mapping=ScalarStride(mapping.scalars(),mapping.stride());
        require(changed!=id,"direction omitted from identity");rejects([&]{WorkUnit::plan(grid,UInt256(),UInt256(1),4,changed);});
        changed=id;changed.algorithm=strided_algorithm(family);rejects([&]{WorkUnit::plan(grid,UInt256(),UInt256(1),4,changed);});
    }
    rejects([&]{strided_algorithm(WorkAlgorithm::DirectMinikeysV1,true);});
    std::cout<<"Reverse progression, inverse, exact work partition and four-family verifier identity passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
