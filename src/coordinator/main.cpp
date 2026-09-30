#include "http.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
namespace {
std::string required(const std::map<std::string,std::string>& args,const char* name){
    const auto it=args.find(name);if(it==args.end())throw std::invalid_argument(std::string("missing --")+name);return it->second;
}
}
int main(int argc,char** argv){
    using namespace keyhunt::coordination;
    try{
        if(argc<2)throw std::invalid_argument("usage: keyhunt-coordinator serve|admin|restore --name VALUE ...; see docs/COORDINATOR.md");
        const std::string action=argv[1];std::set<std::string> allowed;
        if(action=="serve")allowed={"state-dir","api-socket","admin-socket","proxy-uid","authority"};
        else if(action=="admin")allowed={"socket","request"};
        else if(action=="restore")allowed={"source","state-dir"};
        else throw std::invalid_argument("unknown coordinator action");
        std::map<std::string,std::string> args;
        for(int i=2;i<argc;i+=2){const std::string flag=argv[i];
            if(i+1>=argc||flag.rfind("--",0)!=0||!allowed.count(flag.substr(2))||!args.emplace(flag.substr(2),argv[i+1]).second)
                throw std::invalid_argument("invalid or duplicate coordinator option");
        }
        if(action=="restore"){
            keyhunt::storage::Journal::restore(required(args,"source"),required(args,"state-dir"));
            std::cout<<Json({{"restored",true},{"quarantined",true}}).dump()<<'\n';return 0;
        }
        if(action=="admin"){
            std::ifstream file(required(args,"request"),std::ios::binary);
            if(!file)throw std::runtime_error("cannot open admin request file");
            std::string text;char buffer[4096];
            while(file){file.read(buffer,sizeof(buffer));text.append(buffer,size_t(file.gcount()));if(text.size()>8*1024*1024)throw std::invalid_argument("admin request too large");}
            std::cout<<local_admin(required(args,"socket"),parse_json(text)).dump()<<'\n';return 0;
        }
        const auto value=required(args,"proxy-uid");uint64_t uid=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),uid);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||uid>=uint64_t(uid_t(-1)))throw std::invalid_argument("invalid proxy UID");
        return serve({required(args,"state-dir"),required(args,"api-socket"),required(args,"admin-socket"),required(args,"authority"),uid_t(uid)});
    }catch(const std::exception& e){std::cerr<<"keyhunt-coordinator: "<<e.what()<<'\n';return 2;}
}
