#include "fixture.h"
#include "keyhunt/coordinator/offline.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
#include <sys/wait.h>
using namespace cfixture;
using namespace keyhunt::storage::detail;

namespace {
struct Scenario {
    Temporary server,local;
    int64_t now=1800000000,monotonic=boot_seconds();
    Repository repo{server.path.string(),[&]{return now;}};
    Worker worker{local.path.string(),[&]{return now;},[&]{return monotonic;}};
    core::XPointVerifier verifier;
    core::XPointTargets targets=x_targets(verifier,{1,10,11,100});
    Certificate cert;
    Json client,job,config;
    std::string project,path;
    explicit Scenario(uint64_t width=10,uint64_t end=101){
        const auto leaf=pem(1,now-60,now+90*86400);cert=certificate(leaf);
        client=repo.admin({{"operation","bootstrap"},{"name","offline owner"},{"certificate",leaf}});
        project=repo.admin({{"operation","project-create"},{"name","offline test"},{"owner",client["client"]}})["project"];
        auto input=job_input(targets);input["block_width"]=UInt256(width).hex();input["end_exclusive"]=UInt256(end).hex();
        job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",input);
        path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
        config={{"endpoint","https://test.invalid"},{"transport","file"},
                {"jobs",{{{"project",project},{"job",job["job"]},{"devices",{"0","1"}},{"spares",1},{"policy","sequential"}}}}};
        worker.configure(config);
        require(worker.status()["sync_due_in"].is_null(),"file worker advertised automatic synchronization");
    }
    Json relay(const Json& request){return repo.request(cert,"POST","/api/v1/offline-sync",request);}
    void exchange(){worker.import_response(relay(worker.export_request()));}
    void execute_one(const Grant& g){
        CheckpointOptions options;options.xpoint_steps=1;options.checkpoint_seconds=0;
        const auto result=CheckpointRun::xpoint(worker.journal(),g,targets,verifier,
            [&](const auto& batch){return execute(batch,targets,verifier,1024);},options);
        require(result.complete,"offline mock did not complete");
    }
    int64_t deadline(){
        Database db(local.path.string());Statement q(db.handle(),"SELECT min(deadline) FROM worker_grants");
        require(q.step(),"deadline missing");return q.integer(0);
    }
    void audit(){worker.journal().check();repo.admin({{"operation","check"}});}
};
void reservation_and_union(){
    Scenario s;
    rejects([&]{s.worker.synchronize([](const Json&)->Json{throw std::runtime_error("unexpected network");},true);});
    const auto request=s.worker.export_request();
    require(s.worker.export_request()==request,"retry changed exported request");
    require(s.repo.request(s.cert,"GET",s.path+"/status")["assignments"]==0,"request file reserved work before relay");
    const auto response=s.relay(request);
    require(s.repo.request(s.cert,"GET",s.path+"/status")["assignments"]==4,"relay did not reserve all queues/spares");
    require(!s.worker.next("0"),"unimported assignment executable");
    // Another machine cannot claim the four portable grants held in transit.
    Temporary other;Worker peer(other.path.string(),[&]{return s.now;});peer.configure(s.config);
    const auto peer_response=s.relay(peer.export_request());
    rejects([&]{peer.import_response(response);}); // A file is bound to one persisted machine/exchange.
    std::set<std::string> reserved;
    for(const auto& row:response["body"]["value"]["grants"])reserved.insert(row["grant"]["block"].get<std::string>());
    for(const auto& row:peer_response["body"]["value"]["grants"])
        require(!reserved.count(row["grant"]["block"].get<std::string>()),"offline reservations overlap another machine");
    // Return this peer's four unstarted grants through the real authenticated
    // protocol so the first worker can exhaust the entire job below.
    auto returns=peer.export_request()["body"];returns["request"]="return-unused";
    returns["jobs"][0]["devices"]=Json::array();returns["returns"]=Json::array();
    for(const auto& row:peer_response["body"]["value"]["grants"])returns["returns"].push_back(row["grant"]);
    s.repo.request(s.cert,"POST","/api/v1/sync",returns);
    require(s.worker.import_response(response),"first import was a duplicate");
    const auto original_deadline=s.deadline();s.monotonic+=100;s.now+=100;
    require(!s.worker.import_response(response)&&s.deadline()==original_deadline,"duplicate import changed lease");
    auto conflict=response;conflict["body"]["server_time"]=s.now;
    rejects([&]{s.worker.import_response(conflict);});
    for(unsigned cycle=0;cycle<10;++cycle){
        for(const auto* device:{"0","1"})while(const auto grant=s.worker.next(device))s.execute_one(*grant);
        if(s.worker.status()["outbox_bytes"]==0)break;
        const auto exported=s.worker.export_request();
        const auto acknowledgment=s.relay(exported);
        require(s.worker.status()["outbox_bytes"].get<int64_t>()>0,"relay erased local unacknowledged pages");
        s.worker.import_response(acknowledgment);
    }
    require(s.repo.request(s.cert,"GET",s.path+"/status")["finished"]==UInt256(10).hex(),"exact block union incomplete");
    require(s.worker.status()["outbox_bytes"]==0,"final outbox not acknowledged");
    std::set<std::string> found;
    for(const auto& row:s.repo.request(s.cert,"GET",s.path+"/results"))found.insert(row["scalar"].get<std::string>());
    require(found==std::set<std::string>{UInt256(1).hex(),UInt256(10).hex(),UInt256(11).hex(),UInt256(100).hex()},"offline results differ from public fixtures");
    // An old accepted response stays a no-op even after newer exchanges finish.
    require(!s.worker.import_response(response),"old accepted transfer was reactivated");s.audit();
}
void formats_and_deadlines(){
    Scenario s;
    const auto request=s.worker.export_request();const auto origin=s.monotonic;
    s.monotonic+=300;s.now+=300;const auto response=s.relay(request);
    auto bad=response;bad["version"]=2;rejects([&]{s.worker.import_response(bad);});
    bad=response;bad["transfer"]="unknown";rejects([&]{s.worker.import_response(bad);});
    bad=response;bad["request_sha256"]=std::string(64,'0');rejects([&]{s.worker.import_response(bad);});
    bad=response;bad["body"]["value"]["jobs"][0]["target_digest"]=std::string(64,'0');
    rejects([&]{s.worker.import_response(bad);});require(!s.worker.next("0"),"incompatible manifest partly imported");
    bad=response;bad["body"]["value"]["grants"][0]["grant"]["end_exclusive"]=UInt256(1000).hex();
    rejects([&]{s.worker.import_response(bad);});
    s.monotonic+=600;s.now+=600;s.worker.import_response(response);
    require(s.deadline()==origin+2592000-60,"delivery time restarted offline lease");
    s.monotonic=s.deadline();require(!s.worker.next("0"),"elapsed delivery lease still executable");
    // A new delivery attempt can use the same persisted machine snapshot, but
    // the old transfer ID must not gain the refreshed timing origin.
    const auto next=s.worker.export_request();const auto old_response=s.relay(next);
    const auto refreshed=s.worker.export_request(true);
    require(next["transfer"]!=refreshed["transfer"]&&next["body"]==refreshed["body"],"refresh changed machine request");
    rejects([&]{s.worker.import_response(old_response);});
    Database db(s.local.path.string());db.exec("UPDATE worker_file_transfers SET export_boot='previous-boot' WHERE state='pending'");
    rejects([&]{s.worker.import_response(s.relay(refreshed));});
    const auto rebooted=s.worker.export_request();require(rebooted["transfer"]!=refreshed["transfer"],"reboot reused uncertain origin");
    s.worker.import_response(s.relay(rebooted));s.audit();
}
void in_flight_and_migration(){
    Scenario s;s.exchange();
    const auto first=*s.worker.next("0"),second=*s.worker.next("1");
    s.execute_one(first);const auto request=s.worker.export_request();
    s.execute_one(second); // This checkpoint was not in the exported snapshot.
    s.worker.import_response(s.relay(request));
    require(s.worker.status()["outbox_bytes"].get<int64_t>()>0,"import erased newer local checkpoints");
    require(s.repo.request(s.cert,"GET",s.path+"/blocks/"+second.block.hex())["state"]=="in_progress","unexported completion reached server");
    s.exchange();require(s.worker.status()["outbox_bytes"]==0,"next exchange did not acknowledge later checkpoints");s.audit();

    Temporary previous;
    {Database db(previous.path.string());
        // Reconstruct the exact v6 schema by removing only the new empty v7
        // table/migration. Published v1-v6 SQL and checksums remain untouched.
        db.exec("DROP TABLE worker_file_transfers; DELETE FROM migrations WHERE version=7; PRAGMA user_version=6;");}
    {Database upgraded(previous.path.string());upgraded.check();
        Statement version(upgraded.handle(),"PRAGMA user_version");require(version.step()&&version.integer(0)==7,"v6 migration failed");}
    unsigned backups=0;
    for(const auto& entry:std::filesystem::directory_iterator(previous.path)){
        if(entry.is_directory()&&entry.path().filename().string().rfind("pre-v7-",0)==0){
            sqlite3* snapshot=nullptr;
            require(sqlite3_open_v2((entry.path()/"progress.sqlite").c_str(),&snapshot,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open v6 backup");
            {Statement version(snapshot,"PRAGMA user_version");require(version.step()&&version.integer(0)==6,"backup not original v6");}
            sqlite3_close(snapshot);++backups;
        }
    }
    require(backups==1,"missing sealed pre-v7 snapshot");
}
void pages_and_denials(){
    Scenario s(130,131);s.exchange();const auto grant=*s.worker.next("0");s.execute_one(grant);
    const auto request=s.worker.export_request();const auto reply=s.relay(request);
    s.worker.import_response(reply);
    require(s.worker.status()["outbox_bytes"].get<int64_t>()>0,"bounded page erased unsent backlog");
    require(s.repo.request(s.cert,"GET",s.path+"/status")["finished"]==UInt256().hex(),"partial page credited full block");
    for(unsigned page=0;page<3&&s.worker.status()["outbox_bytes"]!=0;++page)s.exchange();
    require(s.worker.status()["outbox_bytes"]==0,"paged upload did not drain");
    require(s.repo.request(s.cert,"GET",s.path+"/results").size()==4,"paged upload lost verified results");
    require(s.repo.request(s.cert,"GET",s.path+"/status")["finished"]==UInt256(1).hex(),"paged union incomplete");s.audit();

    Scenario revoked;revoked.exchange();const auto exported=revoked.worker.export_request();
    revoked.repo.admin({{"operation","credential-set"},{"fingerprint",wire::hex(revoked.cert.fingerprint)},{"enabled",false}});
    denied(401,[&]{revoked.relay(exported);});
    const auto denial=offline_response(exported,401,{{"error","credential revoked"}});
    require(revoked.worker.import_response(denial)&&!revoked.worker.next("0"),"denial did not stop execution");
    require(revoked.worker.status().contains("pause_reason")&&!revoked.worker.import_response(denial),"denial replay changed state");
    revoked.repo.admin({{"operation","credential-set"},{"fingerprint",wire::hex(revoked.cert.fingerprint)},{"enabled",true}});
    revoked.exchange();require(bool(revoked.worker.next("0")),"reviewed credential did not revalidate");
    // Recovery increments the grant generation. A cached portable receipt must
    // be rejected at the server before it can revive a transferred owner.
    const auto old_request=revoked.worker.export_request();const auto old_reply=revoked.relay(old_request);
    const auto old_grant=wire::grant(old_reply["body"]["value"]["grants"][0]["grant"]);
    revoked.repo.request(revoked.cert,"POST",revoked.path+"/blocks/"+old_grant.block.hex()+"/recover",
        {{"client",revoked.client["client"]},{"instance","replacement"},{"device","0"},
         {"request","stopped-recovery"},{"previous_executor_stopped",true}});
    denied(409,[&]{revoked.relay(old_request);});
    revoked.worker.import_response(offline_response(old_request,409,{{"error","superseded generation"}}));
    require(!revoked.worker.next("0"),"stale generation denial did not pause");revoked.audit();
    Scenario expired;const auto pending=expired.worker.export_request();expired.relay(pending);
    expired.now+=2592001;denied(409,[&]{expired.relay(pending);});

}
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
void crashes(){
    for(const std::string stage:{"offline_before_export","offline_after_export","offline_before_import","offline_after_import"}){
        Scenario s;Json request,response;
        const bool importing=stage.find("import")!=std::string::npos;
        if(importing){
            s.exchange();s.execute_one(*s.worker.next("0"));
            request=s.worker.export_request();response=s.relay(request);
            require(s.worker.status()["outbox_bytes"].get<int64_t>()>0,"fault fixture has no pending results");
        }
        const auto pid=fork();require(pid>=0,"fork failed");
        if(pid==0){
            Worker child(s.local.path.string(),[&]{return s.now;},[&]{return s.monotonic;});
            transaction_test_hook=[&](const char* at){if(stage==at)_exit(97);};
            if(importing)child.import_response(response);else child.export_request();
            _exit(98);
        }
        int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==97,"offline fault not reached");
        Worker reopened(s.local.path.string(),[&]{return s.now;},[&]{return s.monotonic;});
        if(importing){
            require((reopened.status()["outbox_bytes"]==0)==(stage=="offline_after_import"),
                    "import crash split outbox deletion from acknowledgment");
            require(reopened.import_response(response)==(stage!="offline_after_import"),"import crash did not preserve atomic receipt");
        }else{
            request=reopened.export_request();require(reopened.export_request()==request,"export crash lost immutable request");
            reopened.import_response(s.relay(request));
        }
        require(bool(reopened.next("0")),"recovered transfer not executable");reopened.journal().check();s.audit();
    }
}
#endif
}
int main(){try{
    reservation_and_union();formats_and_deadlines();in_flight_and_migration();pages_and_denials();
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    crashes();
#endif
    std::cout<<"Offline reservations, exact unions, duplicate imports, delivery deadlines, paging, revocation and generation fences passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
