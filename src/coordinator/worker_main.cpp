#include "keyhunt/coordinator/worker.h"
#include "keyhunt/coordinator/offline.h"
#include "self_test.h"
#include "device_worker.h"
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
        if(argc<2)throw std::invalid_argument("usage: keyhunt-worker configure|configuration|sync|scheduled-sync|file-export|file-relay|file-import|status|next|api|self-test|run-device --name VALUE ...; see docs/COORDINATOR.md");
        const std::string action=argv[1];std::set<std::string> allowed{"state-dir"};
        if(action=="run-device")allowed.insert({"device","queue","backend","once","rebind","table","kernel","group-size","tile-order","ordinal-order","ordinal-seed","ordinal-window","tile-seed","tile-window","batch-size","giant-batch","target-batch","host-memory"});
        else if(action=="configure")allowed.insert("config");
        else if(action=="file-export")allowed.insert({"output","refresh"});
        else if(action=="file-import")allowed.insert({"input","sha256"});
        else if(action=="file-relay")allowed={"config","input","sha256","output"};
        else if(action=="next"||action=="self-test")allowed.insert("device");
        else if(action=="api")allowed.insert("request");
        else if(action!="configuration"&&action!="sync"&&action!="scheduled-sync"&&action!="status")throw std::invalid_argument("unknown worker action");
        std::map<std::string,std::string> args;
        for(int i=2;i<argc;i+=2){const std::string flag=argv[i];
            if(i+1>=argc||flag.rfind("--",0)!=0||!allowed.count(flag.substr(2))||!args.emplace(flag.substr(2),argv[i+1]).second)
                throw std::invalid_argument("invalid or duplicate worker option");
        }
        if(action=="run-device")return run_device(args);
        if(action=="self-test"){
            const auto value=option(args,"device");int device=-1;const auto result=std::from_chars(value.data(),value.data()+value.size(),device);
            if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size()||device<0)throw std::invalid_argument("invalid device ordinal");
            std::cout<<device_self_test(device).dump()<<'\n';return 0;
        }
        if(action=="file-relay"){
            // The courier holds the enrolled credential, not the GPU journal.
            // Pin the input before spending that credential on an HTTPS request.
            const auto config=file(option(args,"config"));
            const auto request=read_offline(option(args,"input"),option(args,"sha256"));
            validate_offline_request(request);
            if(wire::str(request,"endpoint",512)!=wire::str(config,"endpoint",512))
                throw std::invalid_argument("courier authority does not match the offline request");
            Json response;
            try{
                const auto envelope=https_request(config,"POST","/api/v1/offline-sync",request);
                wire::fields(envelope,{"ok","server_time","value"});
                if(!wire::boolean(envelope,"ok"))throw std::runtime_error("offline relay was not acknowledged");
                response=envelope["value"];
            }catch(const Error& error){
                // Carry definite refusals back to stop the disconnected worker.
                // Transport outages and server errors leave the saved request
                // pending and produce no false acknowledgment or denial file.
                response=offline_response(request,error.status,Json{{"error",error.what()}});
            }
            const auto checksum=publish_offline(option(args,"output"),response);
            std::cout<<Json({{"transfer",request["transfer"]},{"sha256",checksum},{"status",response["status"]}}).dump()<<'\n';
            if(!std::cout)throw std::runtime_error("relay output failed; response file may already exist");
            return 0;
        }
        Worker worker(option(args,"state-dir"));Json out;
        if(action=="configure"){worker.configure(file(option(args,"config")));out={{"configured",true}};}
        else if(action=="file-export"){
            const auto refresh=args.count("refresh")?option(args,"refresh"):"no";
            if(refresh!="yes"&&refresh!="no")throw std::invalid_argument("--refresh requires yes or no");
            const auto document=worker.export_request(refresh=="yes");
            out={{"transfer",document["transfer"]},{"request",document["body"]["request"]},
                 {"sha256",publish_offline(option(args,"output"),document)}};
        }
        else if(action=="file-import"){
            const auto response=read_offline(option(args,"input"),option(args,"sha256"));
            const bool applied=worker.import_response(response);
            out={{"applied",applied},{"duplicate",!applied},{"status",response["status"]},{"worker",worker.status()}};
        }
        else if(action=="configuration")out=worker.configuration();
        else if(action=="status")out=worker.status();
        else if(action=="next")out=worker.execution(option(args,"device"));
        else if(action=="api"){
            if(worker.configuration().value("transport",std::string("https"))=="file")
                throw std::runtime_error("file-only worker cannot issue direct HTTPS API requests");
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
