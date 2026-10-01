#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    Temporary server,local;int64_t now=1800000000,monotonic=boot_seconds();
    Repository repo(server.path.string(),[&]{return now;});
    const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
    const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
    const auto project=repo.admin({{"operation","project-create"},{"name","Ethereum"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;std::vector<core::EthereumTarget> values;
    for(unsigned scalar:{1,8,9,16})values.push_back(core::ethereum_target(verifier.derive(UInt256(scalar))));
    const core::EthereumTargets targets(values);const auto input=binding(targets);
    const Json body{{"mode","ethereum"},{"begin",UInt256(1).hex()},{"end_exclusive",UInt256(17).hex()},
        {"block_width",UInt256(8).hex()},{"configuration",wire::hex(input.configuration)},{"targets",wire::hex(input.targets)}};
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",body);
    require(job["mode"]=="ethereum","new job mode silently changed");
    auto bad=body;bad["mode"]="future";denied(400,[&]{repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",bad);});
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
    const Json jobs={{{"project",project},{"job",job["job"]},{"devices",{"gpu0"}},{"spares",1},{"policy","sequential"}}};
    Json old{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},
        {"instance","old-worker"},{"request","old-request"},{"jobs",jobs},{"updates",Json::array()},{"returns",Json::array()}};
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",old);});
    require(repo.request(cert,"GET",path+"/status")["assignments"]==0,"incompatible worker reserved Ethereum work");
    auto bitcoin_only=old;bitcoin_only["capabilities"].push_back("hash160-v1");
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",bitcoin_only);});
    require(repo.request(cert,"GET",path+"/status")["assignments"]==0,"Bitcoin worker reserved Ethereum work");
    auto unknown=old;unknown["capabilities"].push_back("future-v1");
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",unknown);});
    Worker worker(local.path.string(),[&]{return now;},[&]{return monotonic;});
    worker.configure({{"endpoint","https://test.invalid"},{"ca","/private/ca"},{"certificate","/private/certificate"},
                      {"key","/private/key"},{"jobs",jobs}});
    Json request;
    auto transport=[&](const Json& sent){
        require(sent["capabilities"]==Json({"checkpoint-v1","offline-lease-v1","hash160-v1","ethereum-v1","vanity-v1","minikeys-v1","scalar-stride-v1","scalar-reverse-v1"}),"worker omitted Ethereum capability");
        request=sent;const auto response=repo.request(cert,"POST","/api/v1/sync",sent);
        return Json{{"ok",true},{"server_time",now},{"value",response},{"controls",repo.control_snapshot(cert,sent)}};
    };
    rejects([&]{worker.synchronize([&](const Json& sent){auto response=transport(sent);
        response["value"]["jobs"][0]["mode"]="future";return response;});});
    require(!worker.next("gpu0"),"unknown remote mode imported");
    worker.synchronize(transport,true);
    auto downgrade=request;downgrade["capabilities"]=bitcoin_only["capabilities"];
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",downgrade);});
    unsigned completed=0;
    while(auto grant=worker.next("gpu0")){
        require(worker.execution(*grant)["mode"]=="ethereum","execution manifest lost mode");
        CheckpointOptions options;options.xpoint_steps=3;options.candidate_capacity=2;options.checkpoint_seconds=0;
        const auto result=CheckpointRun::ethereum(worker.journal(),*grant,targets,verifier,[&](const auto& batch){
            backend::EthereumResult result{batch,{}};result.device_steps=batch.step_count();
            for(uint64_t i=0;i<batch.step_count();++i){const auto scalar=batch.scalar_at(i);const auto point=verifier.derive(scalar);
                {const auto hash=core::ethereum_target(point);
                    const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),hash);
                    if(found!=targets.values().end()&&*found==hash)result.matches.push_back({scalar,uint32_t(found-targets.values().begin())});}}
            result.candidate_count=result.matches.size();result.overflow=result.candidate_count>2;
            if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
            return result;
        },options);
        require(result.complete,"Ethereum grant incomplete");++completed;
    }
    require(completed==2&&worker.status()["outbox_bytes"].get<int64_t>()>0,"active/spare completion or outbox missing");
    require(repo.request(cert,"GET",path+"/results").empty(),"offline results appeared before synchronization");
    Json first;
    rejects([&]{worker.synchronize([&](const Json& sent)->Json{first=sent;transport(sent);throw std::runtime_error("lost upload reply");},true);});
    const auto rows=repo.request(cert,"GET",path+"/results");require(rows.size()==4,"server lost an address");
    worker.synchronize([&](const Json& sent){require(sent==first,"pending Ethereum retry mutated");return transport(sent);},true);
    require(worker.status()["outbox_bytes"]==0&&!worker.next("gpu0"),"acknowledged Ethereum outbox retained");
    require(repo.request(cert,"GET",path+"/results")==rows,"retry duplicated results");
    worker.journal().check();repo.admin({{"operation","check"}});
    std::cout<<"Ethereum capability fencing, canonical import, address upload and durable retry passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
