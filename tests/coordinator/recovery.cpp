#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    Temporary server,first,destination,backup,restored;
    int64_t now=1800000000;
    Repository repo(server.path.string(),[&]{return now;});
    const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
    const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
    const auto project=repo.admin({{"operation","project-create"},{"name","test"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;const auto targets=x_targets(verifier,{1,50,100});
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",job_input(targets));
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
    const Json config{{"endpoint","https://test.invalid"},{"ca","/ca"},{"certificate","/cert"},{"key","/key"},
        {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"gpu"}},{"spares",0},{"policy","sequential"}}}}};
    Worker owner(first.path.string(),[&]{return now;});owner.configure(config);
    auto transport=[&](const Json& body){
        // Finish the throwing repository call before constructing JSON's nested
        // initializer-list wrappers. This also models a real transport: an HTTP
        // error has no successful response envelope under construction.
        const auto value=repo.request(cert,"POST","/api/v1/sync",body);
        return Json{{"ok",true},{"server_time",now},{"value",value}};
    };
    owner.synchronize(transport);const auto original=*owner.next("gpu");
    bool stop=false;CheckpointControl control;control.poll=[&]{return stop?CheckpointRequest::Stop:CheckpointRequest::Run;};
    CheckpointOptions options;options.xpoint_steps=5;options.checkpoint_seconds=0;
    auto partial=CheckpointRun::xpoint(owner.journal(),original,targets,verifier,[&](const auto& batch){stop=true;return execute(batch,targets,verifier,1024);},options,{},{},control);
    require(!partial.complete&&partial.computed_scalars==UInt256(5),"partial recovery fixture");
    owner.synchronize(transport,true);
    require(repo.request(cert,"GET",path+"/blocks/"+original.block.hex())["covered"].size()==1,"server partial coverage missing");
    // Capture an old snapshot before a later assignment and credential revocation.
    repo.admin({{"operation","backup"},{"destination",backup.path.string()}});
    now+=31*86400;
    rejects([&]{owner.synchronize(transport,true);});
    require(!owner.next("gpu"),"expired work dispatched");
    Worker replacement(destination.path.string(),[&]{return now;});replacement.configure(config);
    Json recovery{{"client",client["client"]},{"instance",replacement.status()["instance"]},
        {"device","gpu"},{"request","transfer"},{"previous_executor_stopped",false}};
    const auto route=path+"/blocks/"+original.block.hex()+"/recover";
    denied(409,[&]{repo.request(cert,"POST",route,recovery);});
    recovery["previous_executor_stopped"]=true;
    const auto transfer=repo.request(cert,"POST",route,recovery);
    require(transfer["grant"]["generation"].get<int64_t>()>original.generation,"transfer did not fence generation");
    require(repo.request(cert,"POST",route,recovery)==transfer,"recovery retry changed ownership");
    rejects([&]{owner.synchronize(transport,true);});
    replacement.synchronize(transport);const auto grant=*replacement.next("gpu");
    require(replacement.journal().block(grant.scope,grant.block).covered.size()==1,"recovery destination lost accepted coverage");
    const auto complete=CheckpointRun::xpoint(replacement.journal(),grant,targets,verifier,[&](const auto& batch){return execute(batch,targets,verifier,1024);},options);
    require(complete.resumed_scalars==UInt256(5)&&complete.computed_scalars==UInt256(5),"recovery recomputed accepted prefix");
    replacement.synchronize(transport,true);
    require(repo.request(cert,"GET",path+"/results/0/1").size()==1,"result pagination lost earlier owner results");
    repo.admin({{"operation","credential-set"},{"fingerprint",client["fingerprint"]},{"enabled",false}});
    Journal::restore(backup.path.string(),restored.path.string());
    Repository old(restored.path.string(),[&]{return now;});
    require(old.admin({{"operation","check"}})["quarantined"]==true,"old snapshot not quarantined");
    denied(503,[&]{old.request(cert,"GET",path+"/status");});
    auto fresh=config["jobs"];Json claim{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},
        {"instance","offline"},{"request","claim"},{"jobs",fresh},{"updates",Json::array()},{"returns",Json::array()}};
    rejects([&]{old.request(cert,"POST","/api/v1/sync",claim);});
    denied(409,[&]{old.admin({{"operation","activate-restore"},{"old_authority_stopped",true},{"all_previous_executors_stopped",false},{"access_review_complete",true}});});
    const auto activated=old.admin({{"operation","activate-restore"},{"old_authority_stopped",true},{"all_previous_executors_stopped",true},{"access_review_complete",true}});
    require(activated["restored_access_disabled"]==true,"restore resurrected old access");
    denied(401,[&]{old.request(cert,"GET",path+"/status");});
    const auto rotation=pem(2,now-60,now+86400);const auto rotated=certificate(rotation);
    old.admin({{"operation","client-set"},{"client",client["client"]},{"enabled",true}});
    old.admin({{"operation","credential-add"},{"client",client["client"]},{"certificate",rotation}});
    denied(401,[&]{old.request(cert,"GET",path+"/status");});
    require(old.request(rotated,"POST","/api/v1/sync",claim)["grants"].size()==1,"reviewed restore did not activate");
    old.admin({{"operation","check"}});replacement.journal().check();

    // Real SQLITE_FULL on the authoritative connection, during a fragmented
    // checkpoint. Verify that accepted coverage and the machine receipt roll back.
    Temporary limited;Repository disk(limited.path.string(),[&]{return now;});
    const auto dclient=disk.admin({{"operation","bootstrap"},{"name","disk"},{"certificate",rotation}});
    const auto dp=disk.admin({{"operation","project-create"},{"name","disk"},{"owner",dclient["client"]}})["project"].get<std::string>();
    auto input=job_input(targets);input["end_exclusive"]=UInt256(100001).hex();input["block_width"]=UInt256(100000).hex();
    const auto dj=disk.request(rotated,"POST","/api/v1/projects/"+dp+"/jobs",input);
    claim["jobs"][0]["project"]=dp;claim["jobs"][0]["job"]=dj["job"];
    const auto dg=wire::grant(disk.request(rotated,"POST","/api/v1/sync",claim)["grants"][0]["grant"]);
    CheckpointData page{dg.block,uint64_t(dg.generation),1,dg.epoch,{},{}};
    for(uint64_t n=1;n<2049;n+=2)page.coverage.emplace_back(UInt256(n),UInt256(n+1));
    claim["request"]="disk-full";claim["updates"]={{{"grant",wire::grant(dg)},{"started",true},{"checkpoints",{wire::hex(encode_checkpoint(page))}}}};
    disk.test_page_limit(true);
    bool full=false;try{disk.request(rotated,"POST","/api/v1/sync",claim);}catch(const std::exception& e){full=std::string(e.what()).find("full")!=std::string::npos;}
    require(full,"SQLITE_FULL injection did not exhaust pages");
    disk.test_page_limit(false);
    const auto dpath="/api/v1/projects/"+dp+"/jobs/"+dj["job"].get<std::string>();
    require(disk.request(rotated,"GET",dpath+"/blocks/"+dg.block.hex())["covered"].empty(),"disk-full published partial coverage");
    disk.request(rotated,"POST","/api/v1/sync",claim);disk.admin({{"operation","check"}});
    std::cout<<"Expired partial recovery, restore quarantine/access reconciliation and SQLITE_FULL rollback passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
