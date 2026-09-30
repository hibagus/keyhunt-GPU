#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
#include <sys/wait.h>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    for(const std::string stage:{"worker_before_send","worker_after_response","worker_before_ack","worker_after_ack"}){
        Temporary server,local;int64_t now=1800000000;
        Repository repo(server.path.string(),[&]{return now;});
        const auto leaf=pem(1,now-60,now+86400);const auto cert=certificate(leaf);
        const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
        const auto project=repo.admin({{"operation","project-create"},{"name","test"},{"owner",client["client"]}})["project"].get<std::string>();
        core::XPointVerifier verifier;const auto targets=x_targets(verifier,{1,100});auto input=job_input(targets);
        // One hundred checkpoints force multiple bounded upload pages.
        input["block_width"]=UInt256(100).hex();
        const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",input);
        Worker worker(local.path.string(),[&]{return now;});
        worker.configure({{"endpoint","https://test.invalid"},{"ca","/ca"},{"certificate","/cert"},{"key","/key"},
            {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"gpu"}},{"spares",0},{"policy","sequential"}}}}});
        auto transport=[&](const Json& body){return Json{{"ok",true},{"server_time",now},{"value",repo.request(cert,"POST","/api/v1/sync",body)}};};
        worker.synchronize(transport);const auto grant=*worker.next("gpu");
        CheckpointOptions options;options.checkpoint_seconds=0;options.xpoint_steps=1;
        CheckpointRun::xpoint(worker.journal(),grant,targets,verifier,[&](const auto& batch){return execute(batch,targets,verifier,1024);},options);
        const auto pid=fork();require(pid>=0,"fork");
        if(pid==0){
            Repository remote(server.path.string(),[&]{return now;});Worker child(local.path.string(),[&]{return now;});
            transaction_test_hook=[&](const char* at){if(stage==at)_exit(97);};
            child.synchronize([&](const Json& body){return Json{{"ok",true},{"server_time",now},{"value",remote.request(cert,"POST","/api/v1/sync",body)}};},true);
            _exit(98);
        }
        int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==97,"worker fault not reached");
        Worker reopened(local.path.string(),[&]{return now;});
        require(reopened.status()["outbox_bytes"].get<int64_t>()>0,"partial page discarded remaining backlog");
        const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
        require(repo.request(cert,"GET",path+"/status")["finished"]==UInt256().hex(),"paged results finished too early");
        // A post-ack crash already durably advanced the cursor. Other crashes
        // retry the immutable first page, then need one more manual page.
        reopened.synchronize(transport,true);
        if(reopened.status()["outbox_bytes"].get<int64_t>())reopened.synchronize(transport,true);
        require(reopened.status()["outbox_bytes"]==0,"acknowledged pages remain pending");
        require(repo.request(cert,"GET",path+"/status")["finished"]==UInt256(1).hex(),"paged upload failed to finish");
        require(repo.request(cert,"GET",path+"/results").size()==2,"paged upload lost results");
        reopened.journal().check();repo.admin({{"operation","check"}});
    }
    std::cout<<"Worker snapshot/response/ack crashes and bounded paging passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
