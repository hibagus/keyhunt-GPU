#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
  for(const std::string key:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
    Temporary server,local;int64_t now=1800000000,monotonic=boot_seconds();
    Repository repo(server.path.string(),[&]{return now;});
    const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
    const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
    const auto project=repo.admin({{"operation","project-create"},{"name","minikeys"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;std::vector<core::MinikeyTarget> values;
    const auto begin=core::minikey_ordinal(key);const auto length=unsigned(key.size());
    const auto point=verifier.derive(*core::minikey_scalar(key));
    for(uint8_t tag:{1,2})values.push_back(core::minikey_target(length,core::hash160_target(point,tag)));
    const core::MinikeyTargets targets(values);const auto input=binding(targets);
    const Json body{{"mode","minikeys"},{"begin",begin.hex()},{"end_exclusive",begin.add(UInt256(1024)).hex()},
        {"block_width",UInt256(512).hex()},{"configuration",wire::hex(input.configuration)},{"targets",wire::hex(input.targets)}};
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",body);
    require(job["mode"]=="minikeys","new job mode silently changed");
    auto outside=body;outside["end_exclusive"]=core::minikey_space_end(length).add(UInt256(1)).hex();
    denied(400,[&]{repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",outside);});
    auto bad_length=body;auto invalid=input.targets;invalid[0]=26;bad_length["targets"]=wire::hex(invalid);
    rejects([&]{repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",bad_length);});
    auto bad=body;bad["mode"]="future";denied(400,[&]{repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",bad);});
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
    const Json jobs={{{"project",project},{"job",job["job"]},{"devices",{"gpu0"}},{"spares",1},{"policy","sequential"}}};
    Json old{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},
        {"instance","old-worker"},{"request","old-request"},{"jobs",jobs},{"updates",Json::array()},{"returns",Json::array()}};
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",old);});
    require(repo.request(cert,"GET",path+"/status")["assignments"]==0,"incompatible worker reserved minikeys work");
    auto previous=old;previous["capabilities"]={"checkpoint-v1","offline-lease-v1","hash160-v1","ethereum-v1","vanity-v1"};
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",previous);});
    require(repo.request(cert,"GET",path+"/status")["assignments"]==0,"older worker reserved minikeys work");
    auto unknown=old;unknown["capabilities"].push_back("future-v1");
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",unknown);});
    Worker worker(local.path.string(),[&]{return now;},[&]{return monotonic;});
    worker.configure({{"endpoint","https://test.invalid"},{"ca","/private/ca"},{"certificate","/private/certificate"},
                      {"key","/private/key"},{"jobs",jobs}});
    Json request;
    auto transport=[&](const Json& sent){
        require(sent["capabilities"]==Json({"checkpoint-v1","offline-lease-v1","hash160-v1","ethereum-v1","vanity-v1","minikeys-v1","scalar-stride-v1","scalar-reverse-v1"}),"worker omitted minikeys capability");
        request=sent;const auto response=repo.request(cert,"POST","/api/v1/sync",sent);
        return Json{{"ok",true},{"server_time",now},{"value",response},{"controls",repo.control_snapshot(cert,sent)}};
    };
    rejects([&]{worker.synchronize([&](const Json& sent){auto response=transport(sent);
        response["value"]["jobs"][0]["mode"]="future";return response;});});
    require(!worker.next("gpu0"),"unknown remote mode imported");
    worker.synchronize(transport,true);
    auto downgrade=request;downgrade["capabilities"]=previous["capabilities"];
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",downgrade);});
    unsigned completed=0;
    while(auto grant=worker.next("gpu0")){
        require(worker.execution(*grant)["mode"]=="minikeys","execution manifest lost mode");
        CheckpointOptions options;options.xpoint_steps=512;options.candidate_capacity=4;options.checkpoint_seconds=0;
        const auto result=CheckpointRun::minikeys(worker.journal(),*grant,targets,verifier,[&](const auto& batch){
            backend::MinikeysResult result{batch,{}};result.device_steps=batch.step_count();
            for(uint64_t i=0;i<batch.step_count();++i){const auto at=batch.ordinal_at(i);
                const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
                const auto pub=verifier.derive(*scalar);
                for(uint8_t tag:{1,2}){const auto target=core::minikey_target(length,core::hash160_target(pub,tag));
                    const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),target);
                    if(found!=targets.values().end()&&*found==target)result.matches.push_back({at,uint32_t(found-targets.values().begin())});}}
            result.candidate_count=result.matches.size();result.overflow=result.candidate_count>4;
            if(result.overflow)result.matches.clear();else result.verified_steps=result.device_steps;
            return result;
        },options);
        require(result.complete,"minikeys grant incomplete");++completed;
    }
    require(completed==2&&worker.status()["outbox_bytes"].get<int64_t>()>0,"active/spare completion or outbox missing");
    require(repo.request(cert,"GET",path+"/results").empty(),"offline results appeared before synchronization");
    Json first;
    rejects([&]{worker.synchronize([&](const Json& sent)->Json{first=sent;transport(sent);throw std::runtime_error("lost upload reply");},true);});
    const auto rows=repo.request(cert,"GET",path+"/results");require(rows.size()==2,"server lost an encoding relation");
    for(const auto& row:rows){
        require(row["ordinal"]==begin.hex()&&row["minikey"]==key&&row["scalar"]==core::minikey_scalar(key)->hex(),"public ordinal/private scalar confused");
        require(row["coordinate_space"]=="minikey-ordinal-v1","missing ordinal coordinate label");
    }
    auto stale=first;stale["capabilities"]=previous["capabilities"];
    denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",stale);});
    worker.synchronize([&](const Json& sent){require(sent==first,"pending minikeys retry mutated");return transport(sent);},true);
    require(worker.status()["outbox_bytes"]==0&&!worker.next("gpu0"),"acknowledged minikeys outbox retained");
    require(repo.request(cert,"GET",path+"/results")==rows,"retry duplicated results");
    worker.journal().check();repo.admin({{"operation","check"}});
    std::cout<<"minikeys capability fencing, canonical import, overlapping prefix/encoding upload and durable retry passed\n";
  }
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
