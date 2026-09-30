#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    Temporary server,local;int64_t now=1800000000;
    Repository repo(server.path.string(),[&]{return now;});
    const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
    const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
    const auto project=repo.admin({{"operation","project-create"},{"name","dispatch"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;const auto targets=x_targets(verifier,{1,11,21,31});
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",job_input(targets));
    Json config{{"endpoint","https://test.invalid"},{"ca","/private/ca"},{"certificate","/private/cert"},{"key","/private/key"},
        {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"gpu0","gpu1"}},{"spares",1},{"policy","sequential"}}}}};
    Worker sync(local.path.string(),[&]{return now;});sync.configure(config);
    sync.synchronize([&](const Json& body){return Json{{"ok",true},{"server_time",now},
        {"value",repo.request(cert,"POST","/api/v1/sync",body)},{"controls",repo.control_snapshot(cert,body)}};});
    Worker fast(local.path.string(),[&]{return now;});fast.acquire_device("gpu0","uuid0");
    std::optional<Grant> held;
    {
        Worker slow(local.path.string(),[&]{return now;});slow.acquire_device("gpu1","uuid1");
        held=slow.claim_device();require(bool(held),"slow device claim");
        rejects([&]{Worker duplicate(local.path.string());duplicate.acquire_device("gpu1","uuid1",true);});
        std::set<UInt256> blocks;
        for(int i=0;i<3;++i){
            const auto g=fast.claim_device();require(g&&g->block!=held->block&&blocks.insert(g->block).second,"active block stolen or duplicate claim");
            require(fast.claim_device()->block==g->block,"claim not idempotent");
            CheckpointRun::xpoint(fast.journal(),*g,targets,verifier,[&](const auto& batch){return execute(batch,targets,verifier,1024);});
        }
        require(!fast.claim_device(),"active block became stealable");
        require(slow.claim_device()->block==held->block,"slow owner lost slot");
    }
    // Replacing a stopped process preserves its unfinished block. A visibility
    // reorder cannot silently bind that queue to another physical GPU.
    rejects([&]{Worker moved(local.path.string());moved.acquire_device("gpu1","replacement");});
    Worker replacement(local.path.string(),[&]{return now;});replacement.acquire_device("gpu1","replacement",true);
    require(replacement.claim_device()->block==held->block,"stopped-device handoff lost block");
    Database db(local.path.string());db.exec("UPDATE worker_grants SET paused=1");
    require(!replacement.claim_device(),"paused block dispatched");
    db.exec("UPDATE worker_grants SET paused=0,deadline=0");
    require(!replacement.claim_device(),"expired block dispatched");
    sync.journal().check();require(sync.status()["outbox_bytes"].get<int64_t>()>0,"dispatch caused hidden upload");
    std::cout<<"transactional claims, queued stealing, active exclusion, UUID handoff and lease fencing passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
