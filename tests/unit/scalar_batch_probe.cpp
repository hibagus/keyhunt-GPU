#include "keyhunt/scheduler/scalar_batch_planner.h"
#include <iostream>
#include <sstream>
using namespace keyhunt;using core::UInt256;using core::ScalarInterval;
std::vector<std::string> split(const std::string& s,char delim){
    std::vector<std::string> result;std::istringstream in(s);std::string v;
    while(std::getline(in,v,delim))result.push_back(v);return result;
}
int main(){std::string row;while(std::getline(std::cin,row)){try{
    std::istringstream in(row);std::string lo,hi,stride,order,works,batches,accepts,gaps;bool reverse,orbit;unsigned limit;
    if(!(in>>lo>>hi>>stride>>reverse>>orbit>>order>>works>>batches>>accepts>>limit>>gaps)||limit>2048)throw std::invalid_argument("probe row");
    const ScalarInterval scalars(UInt256::from_hex(lo),UInt256::from_hex(hi));const auto step=UInt256::from_hex(stride);
    scheduler::ExecutionIdentity id;id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    if(step!=UInt256(1)||reverse||orbit){id.stride_mapping=core::ScalarStride(scalars,step,reverse,orbit);
        id.algorithm=scheduler::strided_algorithm(id.algorithm,reverse,orbit);}
    const auto root=id.stride_mapping?id.stride_mapping->indices():scalars;
    std::vector<ScalarInterval> missing;
    if(gaps!="-")for(const auto& gap:split(gaps,',')){const auto parts=split(gap,':');missing.emplace_back(UInt256::from_hex(parts.at(0)),UInt256::from_hex(parts.at(1)));}
    scheduler::ScalarBatchPlanner planner({root,root.size()},UInt256(),missing,id,scheduler::parse_scalar_batch_order(order));
    const auto ws=split(works,','),bs=split(batches,','),ok=split(accepts,',');std::ostringstream out;out<<"ok";
    for(unsigned i=0;i<limit;++i){const auto plan=planner.plan(UInt256::from_hex(ws.at(i%ws.size())),std::stoull(bs.at(i%bs.size())));if(!plan)break;
        const auto& b=plan->batch;const auto& w=b.work().interval();
        out<<' '<<b.interval().begin().hex()<<':'<<b.interval().end().hex()<<':'<<w.begin().hex()<<':'<<w.end().hex()
           <<':'<<plan->starts_work<<':'<<plan->finishes_work<<':'<<b.scalar_at(0).hex()<<':'<<b.scalar_at(b.step_count()-1).hex()<<':'<<b.orbit_variant();
        if(ok.at(i%ok.size())=="1")planner.accept();
    }
    std::cout<<out.str()<<'\n';
}catch(const std::exception&){std::cout<<"invalid\n";}}}
