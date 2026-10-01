#include "keyhunt/core/minikey_search.h"
#include <iostream>
#include <sstream>
using namespace keyhunt;using core::UInt256;
int main(){std::string row;while(std::getline(std::cin,row)){try{
    std::istringstream in(row);std::string seed,window,order,word;unsigned length,limit,count;
    if(!(in>>length>>seed>>window>>order>>limit>>count)||!count||count>32||limit>1024)throw std::invalid_argument("invalid probe row");
    struct Attempt {UInt256 work;uint64_t steps;bool accept;};std::vector<Attempt> attempts;
    for(unsigned i=0;i<count;++i){uint64_t steps;unsigned accept;
        if(!(in>>word>>steps>>accept)||accept>1)throw std::invalid_argument("missing attempt");
        attempts.push_back({UInt256::from_hex(word),steps,bool(accept)});}
    if(!(in>>count)||count>32)throw std::invalid_argument("invalid gap count");
    std::vector<core::ScalarInterval> gaps;
    for(unsigned i=0;i<count;++i){std::string end;if(!(in>>word>>end))throw std::invalid_argument("missing gap");
        gaps.emplace_back(UInt256::from_hex(word),UInt256::from_hex(end));}
    if(in>>word)throw std::invalid_argument("trailing input");
    const core::ScalarInterval root(UInt256(1),core::minikey_space_end(length));
    scheduler::BlockGrid grid(root,root.size());scheduler::ExecutionIdentity id;
    id.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    core::MinikeyBatchPlanner planner(grid,UInt256(),gaps,id,core::parse_minikey_order(order),core::parse_minikey_random_window(seed,window));
    std::ostringstream out;out<<"ok";
    for(unsigned i=0;i<limit;++i){const auto& attempt=attempts[i%attempts.size()];
        const auto selected=planner.plan(attempt.work,attempt.steps);if(!selected)break;
        const auto& batch=selected->batch;const auto& work=batch.work().interval();
        if(batch.ordinal_at(0)!=batch.interval().begin()||batch.ordinal_at(batch.step_count()-1)!=batch.interval().end().subtract(UInt256(1)))
            throw std::logic_error("random tile lane mapping differs");
        out<<' '<<batch.interval().begin().hex()<<':'<<batch.interval().end().hex()<<':'<<work.begin().hex()<<':'<<work.end().hex()
           <<':'<<selected->starts_work<<':'<<selected->finishes_work<<':'<<batch.ordinal_reverse();
        if(attempt.accept)planner.accept();
    }
    std::cout<<out.str()<<'\n';
}catch(const std::exception&){std::cout<<"invalid\n";}}}
