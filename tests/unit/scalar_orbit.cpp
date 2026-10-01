#include "keyhunt/scheduler/work_unit.h"
#include <iostream>
using namespace keyhunt;using namespace core;using namespace scheduler;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("missing rejection");}
int main(){try{
    // Uneven block/work/batch widths exercise every member boundary, including
    // blocks beginning halfway through a variant. Each coordinate appears once.
    for(bool reverse:{false,true})for(unsigned seeds:{1,2,3,7,17})for(unsigned width:{1,5,23}){
        ScalarStride mapping({UInt256(101),UInt256(101+seeds*7)},UInt256(7),reverse,true);
        for(auto family:{WorkAlgorithm::DirectXPointV1,WorkAlgorithm::DirectHash160V1,WorkAlgorithm::DirectEthereumV1,WorkAlgorithm::DirectVanityV1}){
            ExecutionIdentity id;id.algorithm=strided_algorithm(family,reverse,true);id.stride_mapping=mapping;
            id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
            BlockGrid grid(mapping.indices(),UInt256(width));uint64_t visited=0;
            for(uint64_t block=0;block<grid.count().to_uint64();++block){auto cursor=grid.block(UInt256(block)).begin();
                while(auto work=WorkUnit::plan(grid,UInt256(block),cursor,19,id)){
                    while(auto batch=KernelBatch::plan(*work,cursor,11)){
                        const auto variant=batch->orbit_variant();require(batch->scalar_orbit(),"orbit identity lost");
                        for(uint64_t i=0;i<batch->step_count();++i){
                            const auto coordinate=UInt256(++visited);
                            require(batch->coordinate_at(i)==coordinate,"gap or duplicate coordinate");
                            require(mapping.variant(coordinate)==variant,"batch crossed variant boundary");
                            require(batch->seed_scalar_at(i)==mapping.seed(coordinate),"wrong seed");
                            require(batch->scalar_at(i)==mapping.scalar(coordinate),"wrong private scalar");
                            require(mapping.index(mapping.seed(coordinate),variant)==coordinate,"wrong inverse");
                        }
                        cursor=batch->interval().end();
                    }
                }
            }
            require(visited==6*seeds,"incomplete expanded coverage");
            auto bad=id;bad.algorithm=strided_algorithm(family,reverse);rejects([&]{WorkUnit::plan(grid,UInt256(),UInt256(1),7,bad);});
        }
        rejects([&]{mapping.index(UInt256(101));});rejects([&]{mapping.index(UInt256(101),6);});
    }
    const auto limit=scalar_order().subtract(UInt256(1)).divmod(UInt256(6)).first;
    ScalarStride largest({UInt256(1),limit.add(UInt256(1))},UInt256(1),false,true);
    require(largest.indices().size()==limit.multiply(UInt256(6)),"wrong maximum domain");
    rejects([&]{ScalarStride({UInt256(1),limit.add(UInt256(2))},UInt256(1),false,true);});
    rejects([&]{scalar_orbit(UInt256(),0);});rejects([&]{scalar_orbit(scalar_order(),0);});
    rejects([&]{scalar_orbit(UInt256(1),6);});
    std::cout<<"Orbit mapping, exact variant partitions, bounds and identity checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
