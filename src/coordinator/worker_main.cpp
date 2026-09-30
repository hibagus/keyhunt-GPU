#include "keyhunt/coordinator/worker.h"
#include "self_test.h"
#include "protocol.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
namespace {
using keyhunt::coordination::Json;
std::string option(const std::map<std::string,std::string>& args,const char* name){
    const auto it=args.find(name);if(it==args.end())throw std::invalid_argument(std::string("missing --")+name);return it->second;
}
Json file(const std::string& path){
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot read request/configuration file");
    std::string data;char buffer[4096];while(in){in.read(buffer,sizeof(buffer));data.append(buffer,size_t(in.gcount()));if(data.size()>8*1024*1024)throw std::runtime_error("input file exceeds limit");}
    return keyhunt::coordination::parse_json(data);
}
}
int main(int argc,char** argv){
    using namespace keyhunt::coordination;
    try{
        if(argc<2)throw std::invalid_argument("usage: keyhunt-worker configure|configuration|sync|scheduled-sync|status|next|api|self-test --name VALUE ...; see docs/COORDINATOR.md");
        const std::string action=argv[1];std::set<std::string> allowed{"state-dir"};
        if(action=="configure")allowed.insert("config");
        else if(action=="next"||action=="self-test")allowed.insert("device");
        else if(action=="api")allowed.insert("request");
        else if(action!="configuration"&&action!="sync"&&action!="scheduled-sync"&&action!="status")throw std::invalid_argument("unknown worker action");
        std::map<std::string,std::string> args;
        for(int i=2;i<argc;i+=2){const std::string flag=argv[i];
            if(i+1>=argc||flag.rfind("--",0)!=0||!allowed.count(flag.substr(2))||!args.emplace(flag.substr(2),argv[i+1]).second)
                throw std::invalid_argument("invalid or duplicate worker option");
        }
        if(action=="self-test"){
            const auto value=option(args,"device");int device=-1;const auto result=std::from_chars(value.data(),value.data()+value.size(),device);
            if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size()||device<0)throw std::invalid_argument("invalid device ordinal");
            std::cout<<device_self_test(device).dump()<<'\n';return 0;
        }
        Worker worker(option(args,"state-dir"));Json out;
        if(action=="configure"){worker.configure(file(option(args,"config")));out={{"configured",true}};}
        else if(action=="configuration")out=worker.configuration();
        else if(action=="status")out=worker.status();
        else if(action=="next")out=worker.execution(option(args,"device"));
        else if(action=="api"){
            const auto request=file(option(args,"request"));wire::fields(request,{"method","path"},{"body"});
            out=https_request(worker.configuration(),wire::str(request,"method",8),wire::str(request,"path",4096),request.value("body",Json::object()));
        }else{
            const auto config=worker.configuration();
            const bool sent=worker.synchronize([&](const Json& body){return https_request(config,"POST","/api/v1/sync",body);},action=="sync");
            out=worker.status();out["sent"]=sent;
        }
        std::cout<<out.dump()<<'\n';if(!std::cout)throw std::runtime_error("worker output failed; committed state retained");return 0;
    }catch(const Error& e){std::cerr<<Json({{"error",e.what()},{"status",e.status}}).dump()<<'\n';return 2;}
    catch(const std::exception& e){std::cerr<<Json({{"error",e.what()}}).dump()<<'\n';return 2;}
}
