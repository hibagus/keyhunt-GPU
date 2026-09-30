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
    const auto project=repo.admin({{"operation","project-create"},{"name","test"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;const auto targets=x_targets(verifier,{1,50,100});
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",job_input(targets));
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
    Json config{{"endpoint","https://test.invalid"},{"ca","/private/ca"},{"certificate","/private/certificate"},{"key","/private/key"},
        {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"gpu0","gpu1"}},{"spares",1},{"policy","sequential"}}}}};
    Worker worker(local.path.string(),[&]{return now;},[&]{return monotonic;});worker.configure(config);
    size_t calls=0;Json first_body;
    auto transport=[&](const Json& body){++calls;return Json{{"ok",true},{"server_time",now},{"value",repo.request(cert,"POST","/api/v1/sync",body)}};};
    rejects([&]{worker.synchronize([&](const Json& body)->Json{first_body=body;transport(body);throw std::runtime_error("lost reply");});});
    require(worker.status()["pending_request"]==true&&!worker.next("gpu0"),"unacknowledged grant executed before durable import");
    require(!worker.synchronize(transport)&&calls==1,"unscheduled retry contacted server");
    require(worker.synchronize([&](const Json& body){require(body==first_body,"retry payload mutated");return transport(body);},true),"manual retry not sent");
    const auto g0=*worker.next("gpu0"),g1=*worker.next("gpu1");
    require(g0.block!=g1.block,"mock GPUs received same block");
    rejects([&]{worker.journal().claim(g0.scope,"bypass","claim");});
    rejects([&]{worker.journal().renew(g0,"extend");});
    auto execute_grant=[&](const Grant& g){
        CheckpointOptions options;options.xpoint_steps=3;options.checkpoint_seconds=0;
        const auto result=CheckpointRun::xpoint(worker.journal(),g,targets,verifier,
            [&](const scheduler::KernelBatch& batch){return execute(batch,targets,verifier,1024);},options);
        require(result.complete,"mock execution incomplete");
    };
    execute_grant(g0);
    require(worker.status()["outbox_bytes"].get<int64_t>()>0,"checkpoint missing atomic outbox");
    require(repo.request(cert,"GET",path+"/blocks/"+g0.block.hex())["state"]=="in_progress","local completion changed server without sync");
    const auto before=worker.status()["outbox_bytes"].get<int64_t>();
    monotonic+=100;require(!worker.synchronize(transport)&&calls==2,"GPU completion caused early network traffic");
    worker.synchronize([&](const Json& body){
        // Simulate a GPU checkpoint arriving during network I/O. Its later
        // outbox IDs must survive acknowledgment of the earlier snapshot.
        execute_grant(g1);return transport(body);
    },true);
    require(worker.status()["outbox_bytes"].get<int64_t>()>0&&before>0,"in-flight acknowledgment erased newer checkpoints");
    require(repo.request(cert,"GET",path+"/blocks/"+g0.block.hex())["state"]=="finished","server did not accept complete first block");
    require(repo.request(cert,"GET",path+"/blocks/"+g1.block.hex())["state"]=="in_progress","newer block escaped its snapshot");
    worker.synchronize(transport,true);require(worker.status()["outbox_bytes"]==0,"acknowledged outbox not released");
    require(repo.request(cert,"GET",path+"/results").size()==1,"verified result upload");
    // A saved grant cannot be used through checkpoint CLI after reboot or an
    // elapsed monotonic deadline, even if the wall clock moves backwards.
    const auto next=*worker.next("gpu0");
    Database db(local.path.string());
    db.exec("UPDATE worker_grants SET boot='previous-boot'");
    require(!worker.next("gpu0"),"uncertain boot allowed offline dispatch");
    rejects([&]{execute_grant(next);});
    worker.synchronize(transport,true);require(bool(worker.next("gpu0")),"sync did not revalidate boot");
    db.exec("UPDATE worker_grants SET deadline=0");now-=3600;
    rejects([&]{execute_grant(next);});now+=3600;
    worker.synchronize(transport,true);
    // A bounded outbox failure rolls the entire checkpoint back, including its
    // completion receipt; the caller can safely retry after acknowledged upload.
    rejects([&]{
        CheckpointOptions options;options.checkpoint_seconds=0;options.xpoint_steps=3;
        CheckpointRun::xpoint(worker.journal(),next,targets,verifier,[&](const auto& batch){
            db.exec("UPDATE worker_settings SET outbox_bytes=outbox_limit");
            return execute(batch,targets,verifier,1024);
        },options);
    });
    require(worker.journal().block(next.scope,next.block).state=="in_progress","outbox-full credited coverage");
    db.exec("UPDATE worker_settings SET outbox_bytes=(SELECT COALESCE(sum(length(payload)),0) FROM worker_outbox)");
    worker.journal().check();repo.admin({{"operation","check"}});
    // Reopening uses the persisted schedule, including when queues are empty.
    Worker reopened(local.path.string(),[&]{return now;},[&]{return monotonic;});
    const auto count=calls;require(!reopened.synchronize(transport)&&calls==count,"restart reset contact schedule");
    monotonic+=7200;now+=7200;require(reopened.synchronize(transport)&&calls==count+1,"scheduled machine sync missing");
    std::cout<<"Durable grant import, two mock GPUs, atomic outbox, schedule, retries and offline fencing passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
