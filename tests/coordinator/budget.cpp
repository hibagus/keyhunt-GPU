#include "fixture.h"
#include <chrono>
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    Temporary state;const int64_t now=1800000000;
    Repository repo(state.path.string(),[&]{return now;});
    const auto first=pem(1,now-60,now+86400);const auto root=certificate(first);
    const auto owner=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",first}});
    const auto project=repo.admin({{"operation","project-create"},{"name","budget"},{"owner",owner["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;auto input=job_input(x_targets(verifier,{2000000}));
    input["end_exclusive"]=UInt256(1048577).hex();input["block_width"]=UInt256(1024).hex();
    const auto job=repo.request(root,"POST","/api/v1/projects/"+project+"/jobs",input);
    std::vector<double> claim_ms,progress_ms;std::vector<Certificate> clients;std::vector<Json> bodies,replies;
    auto timed=[&](const Certificate& cert,const Json& body,std::vector<double>& samples){
        const auto begin=std::chrono::steady_clock::now();const auto reply=repo.request(cert,"POST","/api/v1/sync",body);
        samples.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());return reply;
    };
    for(int n=0;n<32;++n){
        const auto leaf=pem(n+2,now-60,now+86400);clients.push_back(certificate(leaf));
        const auto client=repo.admin({{"operation","client-add"},{"name","machine"},{"certificate",leaf}});
        repo.admin({{"operation","membership-set"},{"project",project},{"client",client["client"]},{"role","worker"}});
        Json body{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},{"instance","machine"},{"request","claim"},
            {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"gpu0","gpu1"}},{"spares",1},{"policy","random"}}}},
            {"updates",Json::array()},{"returns",Json::array()}};
        replies.push_back(timed(clients.back(),body,claim_ms));bodies.push_back(body);
    }
    std::set<std::string> seen;
    for(size_t n=0;n<clients.size();++n){
        auto body=bodies[n];body["request"]="progress";
        for(const auto& row:replies[n]["grants"]){
            const auto g=wire::grant(row["grant"]);require(seen.insert(g.block.hex()).second,"burst duplicated a block");
            Json pages=Json::array();
            if(pages.empty()&&body["updates"].empty()){
                CheckpointData data{g.block,uint64_t(g.generation),1,g.epoch,{},{}};
                for(uint64_t i=0;i<256;i+=2)data.coverage.emplace_back(g.interval.begin().add(UInt256(i)),g.interval.begin().add(UInt256(i+1)));
                pages.push_back(wire::hex(encode_checkpoint(data)));
            }
            body["updates"].push_back({{"grant",wire::grant(g)},{"started",!pages.empty()},{"checkpoints",pages}});
        }
        const auto result=timed(clients[n],body,progress_ms);
        require(repo.request(clients[n],"POST","/api/v1/sync",body)==result,"burst retry changed receipt");
    }
    repo.admin({{"operation","check"}});
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>()+"/status";
    bool limited=false;
    for(unsigned i=0;i<130;++i){try{repo.request(clients[0],"GET",path);}catch(const Error& e){require(e.status==429,"unexpected budget error");limited=true;break;}}
    require(limited&&seen.size()==128,"registered client/project budget not enforced");
    auto summarize=[](std::vector<double> values){std::sort(values.begin(),values.end());return Json{{"samples",values.size()},{"median_ms",values[values.size()/2]},{"p95_ms",values[size_t(.95*(values.size()-1))]},{"max_ms",values.back()}};};
    Json report{{"machines",32},{"device_queues",64},{"reserved_blocks",128},{"fragmented_intervals",4096},
        {"claim",summarize(claim_ms)},{"progress_renewal",summarize(progress_ms)},
        {"database_bytes",std::filesystem::file_size(state.path/"progress.sqlite")},
        {"wal_bytes",std::filesystem::file_size(state.path/"progress.sqlite-wal")},
        {"scope","native CPU repository burst; TLS and GPU execution excluded"}};
    std::cout<<report.dump(2)<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
