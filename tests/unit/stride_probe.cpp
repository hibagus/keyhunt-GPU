#include "keyhunt/core/scalar_stride.h"
#include <iostream>
#include <sstream>
using namespace keyhunt::core;
int main(){std::string line;while(std::getline(std::cin,line)){
    try{std::istringstream input(line);std::string op,a,b,s,i,extra;input>>op>>a;
        if(op=="map"){
            input>>b>>s>>i;if(!input||input>>extra)throw std::invalid_argument("fields");
            const ScalarStride mapping({UInt256::from_hex(a),UInt256::from_hex(b)},UInt256::from_hex(s));
            const auto scalar=mapping.scalar(UInt256::from_hex(i));
            std::cout<<mapping.indices().size().hex()<<' '<<scalar.hex()<<' '<<mapping.index(scalar).hex()<<'\n';
        }else if(op=="power"){
            unsigned bit;input>>bit;if(!input||input>>extra)throw std::invalid_argument("fields");
            std::cout<<scalar_stride_power(UInt256::from_hex(a),bit).hex()<<'\n';
        }else throw std::invalid_argument("operation");
    }catch(const std::exception&){std::cout<<"invalid\n";}
}}
