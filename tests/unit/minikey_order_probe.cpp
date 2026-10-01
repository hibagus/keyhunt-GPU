#include "keyhunt/core/minikey_search.h"
#include <iostream>
#include <sstream>
using namespace keyhunt;using core::UInt256;
int main(){std::string row;while(std::getline(std::cin,row)){try{
    std::istringstream input(row);std::string lo,hi,order;unsigned length,limit;uint64_t work_size,batch_size;
    if(!(input>>length>>lo>>hi>>work_size>>batch_size>>order>>limit)||!work_size||!batch_size||limit>1024)throw std::invalid_argument("invalid probe row");
    const core::ScalarInterval interval(UInt256::from_hex(lo),UInt256::from_hex(hi));
    if(interval.end()>core::minikey_space_end(length))throw std::invalid_argument("ordinal domain exceeded");
    scheduler::ExecutionIdentity id;id.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;
    id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    const scheduler::BlockGrid grid(interval,interval.size());
    core::MinikeyBatchPlanner planner(grid,UInt256(),{interval},id,core::parse_minikey_order(order));
    std::ostringstream out;out<<"ok";
    for(unsigned i=0;i<limit;++i){
        const auto selected=planner.plan(UInt256(work_size),batch_size);if(!selected)break;
        const auto& batch=selected->batch;
        const auto first=batch.ordinal_at(0),last=batch.ordinal_at(batch.step_count()-1);
        if(first!=batch.coordinate_at(0))throw std::logic_error("receipt coordinate differs");
        out<<' '<<batch.interval().begin().hex()<<':'<<batch.interval().end().hex()<<':'<<first.hex()<<':'<<last.hex()
           <<':'<<core::minikey_text(first,length)<<':'<<core::minikey_text(last,length);
        planner.accept();
    }
    std::cout<<out.str()<<'\n';
}catch(const std::exception&){std::cout<<"invalid\n";}}}
