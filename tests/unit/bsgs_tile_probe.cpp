#include "keyhunt/core/bsgs_search.h"
#include <iostream>
#include <string>
using namespace keyhunt::core;
int main(){
    std::string begin,end;uint64_t m,giants;unsigned reverse;
    while(std::cin>>begin>>end>>m>>giants>>reverse){
        try{
            if(reverse>1)throw std::invalid_argument("invalid order");
            const auto tile=bsgs_tile({UInt256::from_hex(begin),UInt256::from_hex(end)},m,giants,reverse);
            const BsgsBatch batch(tile,m,0,1,{},{});
            std::cout<<tile.begin().hex()<<' '<<tile.end().hex()<<' '<<batch.giants()<<' '<<batch.last_babies()<<' '
                <<batch.scalar_at(batch.giants()-1,batch.last_babies()-1).hex()<<'\n';
        }catch(const std::exception&){std::cout<<"invalid\n";}
    }
}
