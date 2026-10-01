#include "keyhunt/core/bsgs_search.h"
#include <iostream>
#include <sstream>
using namespace keyhunt::core;
int main(){
    std::string line;
    while(std::getline(std::cin,line)){
        try{
            std::istringstream in(line);std::string order,word;uint64_t m,giants;unsigned limit,count;
            if(!(in>>order>>m>>giants>>limit>>count)||!count||count>32||limit>1024)throw std::invalid_argument("invalid probe row");
            std::vector<UInt256> sizes;
            for(unsigned i=0;i<count;++i){if(!(in>>word))throw std::invalid_argument("missing size");sizes.push_back(UInt256::from_hex(word));}
            if(!(in>>count)||count>32)throw std::invalid_argument("invalid gap count");
            std::vector<ScalarInterval> gaps;
            for(unsigned i=0;i<count;++i){std::string end;if(!(in>>word>>end))throw std::invalid_argument("missing gap");gaps.emplace_back(UInt256::from_hex(word),UInt256::from_hex(end));}
            std::optional<BsgsRandomWindow> random;
            if(in>>word){std::string window;if(!(in>>window))throw std::invalid_argument("missing window");
                random=parse_bsgs_random_window(word,window);
                if(in>>word)throw std::invalid_argument("trailing input");}
            BsgsTilePlanner planner(gaps,m,giants,parse_bsgs_tile_order(order),random);std::ostringstream out;out<<"ok";
            for(unsigned i=0;i<limit;++i){
                const auto tile=planner.next(sizes[i%sizes.size()]);if(!tile)break;
                out<<' '<<tile->interval.begin().hex()<<':'<<tile->interval.end().hex()<<':'
                    <<tile->work.begin().hex()<<':'<<tile->work.end().hex()<<':'<<tile->starts_work<<':'<<tile->finishes_work;
            }
            std::cout<<out.str()<<'\n';
        }catch(const std::exception&){std::cout<<"invalid\n";}
    }
}
