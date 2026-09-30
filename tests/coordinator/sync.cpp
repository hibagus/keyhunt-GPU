#include "fixture.h"
#include <future>
#include <iostream>
#include <sys/wait.h>
using namespace cfixture;
using namespace keyhunt::storage::detail;

namespace {
Json batch(const Json& job, const std::string& instance, const std::string& request,
           Json devices = {"gpu0", "gpu1"}, int spares = 1) {
    return {{"protocol", 1}, {"capabilities", {"checkpoint-v1", "offline-lease-v1"}},
            {"instance", instance}, {"request", request}, {"jobs", {{{"project", job["project"]},
            {"job", job["job"]}, {"devices", devices}, {"spares", spares}, {"policy", "sequential"}}}},
            {"updates", Json::array()}, {"returns", Json::array()}};
}
Json update(const Grant& grant, std::vector<ScalarInterval> intervals,
            std::vector<core::XPointMatch> matches = {}) {
    CheckpointData page{grant.block, uint64_t(grant.generation), 1, grant.epoch, intervals, matches};
    return {{"grant", wire::grant(grant)}, {"started", true}, {"checkpoints", {wire::hex(encode_checkpoint(page))}}};
}
struct Setup {
    Temporary temp; int64_t now=1800000000; std::string leaf=pem(1,now-60,now+90*86400);
    Certificate cert=certificate(leaf); Repository repo{temp.path.string(),[this]{return now;}};
    core::XPointVerifier verifier; core::XPointTargets targets=x_targets(verifier,{1,50,100});
    Json client,job;
    Setup(){
        client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
        const auto project=repo.admin({{"operation","project-create"},{"name","test"},{"owner",client["client"]}})["project"].get<std::string>();
        job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",job_input(targets));
    }
    Json sync(const Json& body){return repo.request(cert,"POST","/api/v1/sync",body);}
    std::string path()const{return "/api/v1/projects/"+job["project"].get<std::string>()+"/jobs/"+job["job"].get<std::string>();}
};
void normal(){
    Setup s;const auto request=batch(s.job,"machine-a","initial");const auto first=s.sync(request);
    require(first["grants"].size()==4,"two devices did not receive active/spare queues");
    std::set<std::string> blocks;
    for(const auto& row:first["grants"])require(blocks.insert(row["grant"]["block"].get<std::string>()).second,"duplicate device block");
    s.now+=20*86400;
    require(s.sync(request)==first,"retry altered grants/deadline or response");
    auto changed=request;changed["jobs"][0]["spares"]=0;denied(409,[&]{s.sync(changed);});
    auto incompatible=request;incompatible["protocol"]=2;denied(426,[&]{s.sync(incompatible);});
    const auto grant=wire::grant(first["grants"][0]["grant"]);
    auto renewal=batch(s.job,"machine-a","renew",Json::array());
    renewal["updates"].push_back({{"grant",wire::grant(grant)},{"started",false},{"checkpoints",Json::array()}});
    const auto renewed=s.sync(renewal);
    require(renewed["grants"][0]["grant"]["expires"].get<int64_t>()==s.now+30*86400,"20-day pause renewal failed");
    s.now+=100;require(s.sync(renewal)==renewed,"lost renewal response extended deadline");
    auto forged=renewal;forged["request"]="forged";forged["updates"][0]["grant"]["owner"]="other";
    denied(403,[&]{s.sync(forged);});
    auto progress=batch(s.job,"machine-a","partial",Json::array());
    // A match for scalar 1 uses the canonical sorted target index.
    const auto pub=s.verifier.derive(UInt256(1));core::XPointBytes x;std::copy_n(pub.begin()+1,32,x.begin());
    const auto target=uint32_t(std::lower_bound(s.targets.values().begin(),s.targets.values().end(),x)-s.targets.values().begin());
    progress["updates"].push_back(update(grant,{ScalarInterval(UInt256(1),UInt256(6))},{{UInt256(1),target}}));
    auto bad=progress;auto invalid=decode_checkpoint(wire::unhex(bad["updates"][0]["checkpoints"][0]));
    invalid.matches[0].scalar=UInt256(2);bad["updates"][0]["checkpoints"][0]=wire::hex(encode_checkpoint(invalid));
    rejects([&]{s.sync(bad);});
    require(s.repo.request(s.cert,"GET",s.path()+"/results").empty(),"unverified result survived rollback");
    s.sync(progress);
    require(s.repo.request(s.cert,"GET",s.path()+"/blocks/"+grant.block.hex())["state"]=="in_progress","partial became finished");
    auto done=batch(s.job,"machine-a","done",Json::array());
    done["updates"].push_back(update(grant,{ScalarInterval(UInt256(6),UInt256(11))}));
    const auto finish=s.sync(done);require(finish["accepted"][0]["complete"]==true,"full union not finished");
    require(s.sync(done)==finish,"completion replay failed");
    require(s.repo.request(s.cert,"GET",s.path()+"/results").size()==1,"result missing or duplicated");
    auto returns=batch(s.job,"machine-a","return",Json::array());returns["returns"].push_back(first["grants"][1]["grant"]);
    s.sync(returns);
    s.repo.admin({{"operation","check"}});
    // Revocation must apply even to a receipt created by this credential.
    s.repo.admin({{"operation","credential-set"},{"fingerprint",s.client["fingerprint"]},{"enabled",false}});
    denied(401,[&]{s.sync(request);});
}
void isolation_and_concurrency(){
    Setup s;const auto leaf=pem(2,s.now-60,s.now+90*86400);const auto cert=certificate(leaf);
    const auto other=s.repo.admin({{"operation","client-add"},{"name","other"},{"certificate",leaf}});
    const auto project=s.repo.admin({{"operation","project-create"},{"name","other"},{"owner",other["client"]}})["project"].get<std::string>();
    const auto job=s.repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",job_input(s.targets));
    require(job["job"]==s.job["job"],"identical project manifests differ");
    auto mixed=batch(s.job,"a","mixed");mixed["jobs"].push_back(batch(job,"a","x",Json::array())["jobs"][0]);
    denied(404,[&]{s.sync(mixed);});
    require(s.repo.request(s.cert,"GET",s.path()+"/status")["assignments"]==0,"mixed-project request partially committed");
    s.repo.admin({{"operation","membership-set"},{"project",s.job["project"]},{"client",other["client"]},{"role","reader"}});
    denied(403,[&]{s.repo.request(cert,"POST","/api/v1/sync",batch(s.job,"other","reader"));});
    s.repo.admin({{"operation","membership-set"},{"project",s.job["project"]},{"client",other["client"]},{"role","worker"}});
    auto claim=[&](const Certificate& credential,const std::string& name){
        Repository connection(s.temp.path.string(),[&]{return s.now;});
        return connection.request(credential,"POST","/api/v1/sync",batch(s.job,name,"concurrent"));
    };
    auto one=std::async(std::launch::async,claim,s.cert,"one"),two=std::async(std::launch::async,claim,cert,"two");
    const auto a=one.get(),b=two.get();std::set<std::string> seen;
    for(const auto& result:{a,b})for(const auto& row:result["grants"])
        require(seen.insert(row["grant"]["block"].get<std::string>()).second,"concurrent clients received duplicate blocks");
    require(seen.size()==8,"concurrent queue count");
    s.now+=30*86400+1;
    auto late=batch(s.job,"one","late",Json::array());
    late["updates"].push_back({{"grant",a["grants"][0]["grant"]},{"started",false},{"checkpoints",Json::array()}});
    denied(409,[&]{s.sync(late);});
    const auto next=s.sync(batch(s.job,"new","new",{"gpu"},0));
    require(next["grants"].size()==1&&!seen.count(next["grants"][0]["grant"]["block"]),"expired assignment became normally claimable");
    s.repo.admin({{"operation","check"}});
}
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
void faults(){
    for(const std::string stage:{"after_results","after_coverage","before_commit","after_commit"}){
        Setup s;const auto claimed=s.sync(batch(s.job,"fault","claim",{"gpu"},0));
        const auto g=wire::grant(claimed["grants"][0]["grant"]);
        auto body=batch(s.job,"fault","finish",{"gpu"},0);body["updates"].push_back(update(g,{g.interval}));
        const auto pid=fork();require(pid>=0,"fork");
        if(pid==0){
            Repository child(s.temp.path.string(),[&]{return s.now;});
            transaction_test_hook=[&](const char* at){if(stage==at)_exit(97);};
            child.request(s.cert,"POST","/api/v1/sync",body);_exit(98);
        }
        int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==97,"fault stage not reached");
        Repository reopened(s.temp.path.string(),[&]{return s.now;});
        const auto before=reopened.request(s.cert,"GET",s.path()+"/status");
        require(before["finished"]==UInt256(stage=="after_commit"?1:0).hex(),"crash exposed partial machine transaction");
        const auto response=reopened.request(s.cert,"POST","/api/v1/sync",body);
        require(response["grants"].size()==1&&response["grants"][0]["grant"]["block"]==UInt256(1).hex(),"retry allocated another replacement");
        require(reopened.request(s.cert,"POST","/api/v1/sync",body)==response,"fault retry response changed");
        reopened.admin({{"operation","check"}});
    }
}
#endif
}
int main(){try{
    normal();isolation_and_concurrency();
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    faults();
#endif
    std::cout<<"Machine sync, exact retries, verified progress, ownership, isolation and lease clocks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
