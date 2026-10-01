#include "keyhunt/core/minikey_search.h"
#include <iostream>
#include <sstream>
using namespace keyhunt;using core::UInt256;
int main(){std::string row;while(std::getline(std::cin,row)){try{
    std::istringstream input(row);std::string lo,hi,order;unsigned length,limit;uint64_t work_size,batch_size;
    if(!(input>>length>>lo>>hi>>work_size>>batch_size>>order>>limit)||!work_size||!batch_size||limit>1024)throw std::invalid_argument("invalid probe row");
    const core::ScalarInterval interval(UInt256::from_hex(lo),UInt256::from_hex(hi));
    if(interval.end()>core::minikey_space_end(length))throw std::invalid_argument("ordinal domain exceeded");
    const bool reverse=core::parse_minikey_order(order);scheduler::ExecutionIdentity id;
    id.algorithm=reverse?scheduler::WorkAlgorithm::ReverseMinikeysV1:scheduler::WorkAlgorithm::DirectMinikeysV1;
    id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    const scheduler::BlockGrid grid(interval,interval.size());auto cursor=reverse?interval.end():interval.begin();
    std::optional<scheduler::WorkUnit> work;std::ostringstream out;out<<"ok";
    for(unsigned i=0;i<limit;++i){
        if(!work||cursor==(reverse?work->interval().begin():work->interval().end()))work=scheduler::WorkUnit::plan(grid,UInt256(),cursor,work_size,id);
        if(!work)break;
        const auto batch=*scheduler::KernelBatch::plan(*work,cursor,batch_size);
        const auto first=batch.ordinal_at(0),last=batch.ordinal_at(batch.step_count()-1);
        if(first!=batch.coordinate_at(0))throw std::logic_error("receipt coordinate differs");
        out<<' '<<batch.interval().begin().hex()<<':'<<batch.interval().end().hex()<<':'<<first.hex()<<':'<<last.hex()
           <<':'<<core::minikey_text(first,length)<<':'<<core::minikey_text(last,length);
        cursor=reverse?batch.interval().begin():batch.interval().end();
    }
    std::cout<<out.str()<<'\n';
}catch(const std::exception&){std::cout<<"invalid\n";}}}
