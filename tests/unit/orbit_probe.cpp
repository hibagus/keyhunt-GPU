#include "keyhunt/core/scalar_stride.h"
#include <iostream>
#include <sstream>
using namespace keyhunt::core;
int main(){
    for(std::string line;std::getline(std::cin,line);){try{
        std::istringstream in(line);std::string begin,end,stride,index;unsigned reverse;
        if(!(in>>begin>>end>>stride>>reverse>>index)||reverse>1)throw std::invalid_argument("invalid request");
        ScalarStride mapping({UInt256::from_hex(begin),UInt256::from_hex(end)},UInt256::from_hex(stride),reverse,true);
        const auto at=UInt256::from_hex(index),seed=mapping.seed(at);const auto variant=mapping.variant(at);
        std::cout<<mapping.indices().end().hex()<<' '<<seed.hex()<<' '<<variant<<' '<<mapping.scalar(at).hex()<<' '
            <<mapping.index(seed,variant).hex()<<' '<<mapping.variant_end(at).hex()<<'\n';
    }catch(const std::exception&){std::cout<<"invalid\n";}}
}
