#include "keyhunt/coordinator/worker.h"
#include "protocol.h"
#include <map>
namespace keyhunt::coordination {
using namespace storage;
using namespace storage::detail;
using namespace wire;
struct Worker::Impl {
    Journal journal;
    Database& db;
    Timer timer;
    Impl(const std::string& path,Journal::Clock clock,Timer time):journal(path,std::move(clock)),db(journal.database()),timer(std::move(time)){
        if(!timer)timer=boot_seconds;
    }
    Json configuration()const{
        Statement q(db.handle(),"SELECT configuration FROM worker_settings WHERE singleton=1");
        if(!q.step())throw std::runtime_error("worker is not configured");
        return parse_json(q.text(0));
    }
    std::optional<Json> prepare(bool manual){
        Transaction tx(db);db.writable();
        const auto config=configuration();
        Statement settings(db.handle(),"SELECT instance,schedule_boot,next_sync FROM worker_settings WHERE singleton=1");settings.step();
        if(!manual&&settings.text(1)==boot_id()&&timer()<settings.integer(2)){tx.commit();return {};}
        Statement schedule(db.handle(),"UPDATE worker_settings SET schedule_boot=?,next_sync=? WHERE singleton=1");
        schedule.bind(1,boot_id());schedule.bind(2,timer()+7200);schedule.step();
        Statement pending(db.handle(),"SELECT request,body FROM worker_requests WHERE acknowledged=0");
        if(pending.step()){
            // An attempted retry may update transport timing, never its payload.
            Statement stamp(db.handle(),"UPDATE worker_requests SET sent_boot=?,sent_at=? WHERE request=?");
            stamp.bind(1,boot_id());stamp.bind(2,timer());stamp.bind(3,pending.text(0));stamp.step();
            auto body=parse_json(pending.text(1));tx.commit();return body;
        }
        const auto request=uuid();
        Json body{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},
            {"instance",settings.text(0)},{"request",request},{"jobs",config["jobs"]},
            {"updates",Json::array()},{"returns",Json::array()}};
        std::map<std::string,size_t> entries;
        Statement grants(db.handle(),"SELECT project,job,block,remote FROM worker_grants WHERE acknowledged=0 ORDER BY project,job,block");
        while(grants.step()){
            const auto remote=parse_json(grants.text(3));const auto g=wire::grant(remote);
            const auto state=journal.block(g.scope,g.block);
            const auto key=g.scope.project+hex(bytes(g.scope.job))+g.block.hex();
            entries[key]=body["updates"].size();
            body["updates"].push_back({{"grant",remote},{"started",state.started||state.state=="finished"},{"checkpoints",Json::array()}});
        }
        int64_t through=0;size_t matches=0;
        Statement pages(db.handle(),"SELECT id,project,job,block,generation,payload,checksum FROM worker_outbox ORDER BY id LIMIT 64");
        while(pages.step()){
            const auto key=pages.text(1)+hex(pages.blob(2))+pages.wide(3).hex();
            const auto found=entries.find(key);if(found==entries.end())throw std::runtime_error("outbox has no owned grant");
            auto& update=body["updates"][found->second];const auto g=wire::grant(update["grant"]);
            if(storage::detail::digest(pages.blob(5))!=pages.blob(6))throw std::runtime_error("worker outbox checksum mismatch");
            auto data=decode_checkpoint(pages.blob(5));
            if(data.generation!=uint64_t(pages.integer(4))||data.generation!=uint64_t(g.generation))throw std::runtime_error("outbox generation needs reconciliation");
            if(matches+data.matches.size()>4096)break;
            matches+=data.matches.size();data.epoch=g.epoch;
            update["checkpoints"].push_back(hex(encode_checkpoint(data)));through=pages.integer(0);
        }
        const auto encoded=body.dump();if(encoded.size()>8*1024*1024)throw std::runtime_error("worker snapshot exceeds sync page");
        Statement save(db.handle(),"INSERT INTO worker_requests VALUES(?,?,?,?,?,'',0)");
        save.bind(1,request);save.bind(2,encoded);save.bind(3,through);save.bind(4,boot_id());save.bind(5,timer());save.step();
        // This COMMIT precedes all network I/O. A crash or lost response leaves
        // the exact request available for a later scheduled/manual retry.
        tx.commit();return body;
    }
    void accept(const Json& envelope,const std::string& request){
        fields(envelope,{"ok","server_time","value"},{"controls"});
        if(!boolean(envelope,"ok"))throw std::runtime_error("sync was not acknowledged");
        const auto server_time=integer(envelope,"server_time",1);const auto& value=envelope["value"];
        fields(value,{"protocol","client","instance","request","epoch","issued_at","sync_seconds","lifetime_seconds","accepted","jobs","grants"});
        if(integer(value,"protocol")!=1||str(value,"request",64)!=request||integer(value,"sync_seconds")!=7200||integer(value,"lifetime_seconds")!=2592000)
            throw std::runtime_error("incompatible sync acknowledgment");
        Transaction tx(db);db.writable();
        Statement saved(db.handle(),"SELECT body,through_id,sent_boot,sent_at,acknowledged FROM worker_requests WHERE request=?");saved.bind(1,request);
        if(!saved.step())throw std::runtime_error("unknown sync acknowledgment");
        if(saved.integer(4)){tx.commit();return;}
        if(saved.text(2)!=boot_id())throw std::runtime_error("boot changed during synchronization");
        const auto body=parse_json(saved.text(0));
        if(value["instance"]!=body["instance"])throw std::runtime_error("worker instance changed in response");
        const auto epoch=unhex(str(value,"epoch",32),16);const auto client=str(value,"client",36);
        Statement settings(db.handle(),"SELECT client,epoch,last_ack FROM worker_settings WHERE singleton=1");settings.step();
        if(server_time<settings.integer(2))throw std::runtime_error("coordinator clock regressed; deadline revalidation required");
        if((!settings.text(0).empty()&&settings.text(0)!=client)||(!settings.blob(1).empty()&&settings.blob(1)!=epoch))
            throw std::runtime_error("coordinator/client identity changed; explicit reconciliation required");
        std::map<std::pair<std::string,Digest>,Json> jobs;
        if(!value["jobs"].is_array()||value["jobs"].size()!=body["jobs"].size())throw std::runtime_error("invalid job acknowledgment");
        for(const auto& info:value["jobs"]){
            const auto scope=wire::scope(info);bool requested=false;
            for(const auto& row:body["jobs"])if(wire::scope(row).project==scope.project&&wire::scope(row).job==scope.job)requested=true;
            if(!requested||!jobs.emplace(std::make_pair(scope.project,scope.job),info).second)throw std::runtime_error("unrequested response job");
        }
        if(envelope.contains("controls")){
            if(!envelope["controls"].is_array()||envelope["controls"].size()!=jobs.size())throw std::runtime_error("invalid current controls");
            std::set<std::pair<std::string,Digest>> seen;
            for(const auto& control:envelope["controls"]){
                fields(control,{"project","job","paused"});const auto scope=wire::scope(control);const auto key=std::make_pair(scope.project,scope.job);
                if(!jobs.count(key)||!seen.insert(key).second)throw std::runtime_error("invalid current control scope");
                jobs.at(key)["paused"]=boolean(control,"paused");
            }
        }
        if(!value["grants"].is_array()||value["grants"].size()>128)throw std::runtime_error("invalid grant page");
        for(const auto& row:value["grants"]){
            fields(row,{"device","grant"},{"covered"});const auto g=wire::grant(row["grant"]);
            const auto found=jobs.find({g.scope.project,g.scope.job});
            if(found==jobs.end()||g.owner!=client+"."+str(body,"instance",36)||g.epoch!=epoch)throw std::runtime_error("grant identity mismatch");
            const auto& info=found->second;const auto mode=str(info,"mode",8);
            if(mode!="xpoint"&&mode!="bsgs")throw std::runtime_error("unsupported remote mode");
            Manifest manifest{mode=="xpoint"?Mode::XPoint:Mode::Bsgs,
                ScalarInterval(wide(str(info,"begin",66)),wide(str(info,"end_exclusive",66))),wide(str(info,"block_width",66)),
                wire::digest(str(info,"target_digest",64)),wire::digest(str(info,"algorithm_digest",64))};
            const auto binding=decode_binding(manifest,unhex(str(info,"configuration",100),50),unhex(str(info,"targets",4*1024*1024)));
            // Fresh envelope time prevents an old cached receipt from extending
            // offline execution. Subtract 60 seconds for bounded drain/transport.
            const auto remaining=std::max(int64_t(0),std::min(int64_t(2592000),g.expires-server_time)-60);
            if(row.contains("covered")&&(!row["covered"].is_array()||row["covered"].size()>1024))throw std::runtime_error("invalid accepted coverage page");
            std::vector<ScalarInterval> accepted;
            for(const auto& interval:row.value("covered",Json::array())){fields(interval,{"begin","end_exclusive"});
                accepted.emplace_back(wide(str(interval,"begin",66)),wide(str(interval,"end_exclusive",66)));
                if(!g.interval.contains(accepted.back()))throw std::runtime_error("accepted coverage escaped grant");
            }
            journal.import_remote(g,manifest,binding,row["grant"].dump(),str(row,"device",128),
                saved.integer(3)+remaining,std::max(int64_t(1),journal.timestamp()+remaining),boolean(info,"paused"),accepted);
        }
        for(const auto& row:value["accepted"]){
            fields(row,{"project","job","block","complete"});
            if(!boolean(row,"complete"))continue;
            const auto scope=wire::scope(row);if(!jobs.count({scope.project,scope.job}))throw std::runtime_error("unrequested completion scope");
            const auto block=wide(str(row,"block",66));
            if(journal.block(scope,block).state!="finished")throw std::runtime_error("server finished work missing from local journal");
            Statement done(db.handle(),"UPDATE worker_grants SET acknowledged=1 WHERE project=? AND job=? AND block=?");bind_scope(done,scope);done.bind(3,block);done.step();
        }
        // Delete only pages represented in this immutable acknowledged snapshot;
        // checkpoints committed by the GPU during HTTPS stay in the next page.
        Statement freed(db.handle(),"SELECT COALESCE(sum(length(payload)),0) FROM worker_outbox WHERE id<=?");freed.bind(1,saved.integer(1));freed.step();
        Statement erase(db.handle(),"DELETE FROM worker_outbox WHERE id<=?");erase.bind(1,saved.integer(1));erase.step();
        Statement state(db.handle(),"UPDATE worker_settings SET client=?,epoch=?,last_ack=?,outbox_bytes=outbox_bytes-? WHERE singleton=1");
        state.bind(1,client);state.bind(2,epoch);state.bind(3,server_time);state.bind(4,freed.integer(0));state.step();
        Statement receipt(db.handle(),"UPDATE worker_requests SET response=?,acknowledged=1 WHERE request=?");receipt.bind(1,envelope.dump());receipt.bind(2,request);receipt.step();
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
        if(transaction_test_hook)transaction_test_hook("worker_before_ack");
#endif
        db.exec("DELETE FROM metadata WHERE key='worker_sync_denial'");
        tx.commit();
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
        if(transaction_test_hook)transaction_test_hook("worker_after_ack");
#endif
    }
};
Worker::Worker(const std::string& path,Journal::Clock clock,Timer timer):impl_(std::make_unique<Impl>(path,std::move(clock),std::move(timer))){}
Worker::~Worker()=default;
Journal& Worker::journal(){return impl_->journal;}
Json Worker::configuration()const{return impl_->configuration();}
void Worker::configure(const Json& config){
    fields(config,{"endpoint","ca","certificate","key","jobs"},{"resolve","outbox_limit"});
    const auto endpoint=str(config,"endpoint",512);
    if(endpoint.rfind("https://",0)!=0||endpoint.find_first_of("/?#@",8)!=std::string::npos)throw Error(400,"endpoint must be an HTTPS authority without path or userinfo");
    for(const auto* name:{"ca","certificate","key"}){const auto path=str(config,name,4096);if(path[0]!='/')throw Error(400,"credential paths must be absolute");}
    if(!config["jobs"].is_array()||config["jobs"].empty()||config["jobs"].size()>64)throw Error(400,"worker needs 1..64 job queues");
    std::set<std::string> devices;
    for(const auto& job:config["jobs"]){
        fields(job,{"project","job","devices","spares","policy"});wire::scope(job);integer(job,"spares",0,1);
        const auto policy=str(job,"policy",16);if(policy!="sequential"&&policy!="random"&&policy!="random-window")throw Error(400,"invalid queue policy");
        if(!job["devices"].is_array()||job["devices"].size()>64)throw Error(400,"invalid devices");
        for(const auto& device:job["devices"]){if(!device.is_string())throw Error(400,"invalid device");const auto id=device.get<std::string>();token(id);if(!devices.insert(id).second)throw Error(400,"duplicate device queue");}
    }
    if(devices.empty()||devices.size()>64)throw Error(400,"worker needs 1..64 unique devices");
    const auto limit=config.contains("outbox_limit")?integer(config,"outbox_limit",1048576,1073741824):67108864;
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();
    Statement count(s.db.handle(),"SELECT count(*) FROM worker_settings");count.step();if(count.integer(0))throw Error(409,"worker already configured");
    Statement save(s.db.handle(),"INSERT INTO worker_settings VALUES(1,?,?,'',X'',0,'',0,0,?)");
    save.bind(1,uuid());save.bind(2,config.dump());save.bind(3,limit);save.step();tx.commit();
}
bool Worker::synchronize(const Transport& transport,bool manual){
    auto& s=*impl_;const auto request=s.prepare(manual);if(!request)return false;
    #ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("worker_before_send");
#endif
    Json response;
    try{response=transport(*request);}
    catch(const Error& error){
        if(error.status==401||error.status==403||error.status==404||error.status==409||error.status==426){
            // A definite online authorization/fencing refusal differs from a
            // transport outage. Stop this machine at the next bounded checkpoint
            // boundary while retaining its immutable request and all upload data.
            Transaction tx(s.db);s.db.exec("UPDATE worker_grants SET paused=1 WHERE acknowledged=0");
            const std::string reason=std::to_string(error.status)+": "+error.what();
            s.db.metadata("worker_sync_denial",Bytes(reason.begin(),reason.end()));tx.commit();
        }
        throw;
    }
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("worker_after_response");
#endif
    s.accept(response,str(*request,"request",64));return true;
}
std::optional<Grant> Worker::next(const std::string& device)const{
    token(device);auto& s=*impl_;Transaction tx(s.db,false);s.db.writable();
    Statement q(s.db.handle(),"SELECT project,job,block FROM worker_grants WHERE device=? AND acknowledged=0 AND paused=0 AND boot=? AND deadline>? ORDER BY generation");
    q.bind(1,device);q.bind(2,boot_id());q.bind(3,s.timer());
    while(q.step()){
        const Scope scope{q.text(0),wire::digest(hex(q.blob(1)))};const auto state=s.journal.block(scope,q.wide(2));
        if(state.assignment&&!state.expired){auto result=state.assignment;tx.commit();return result;}
    }
    tx.commit();return {};
}
Json Worker::execution(const std::string& device)const{
    const auto selected=next(device);if(!selected)return nullptr;
    const auto& g=*selected;auto& s=*impl_;
    Statement inputs(s.db.handle(),"SELECT configuration,targets FROM search_bindings WHERE project=? AND job=?");bind_scope(inputs,g.scope);
    if(!inputs.step())throw std::runtime_error("imported grant missing inputs");
    const auto manifest=s.journal.manifest(g.scope);
    const auto token="v1:"+g.scope.project+":"+hex(bytes(g.scope.job))+":"+g.owner+":"+hex(g.epoch)+":"+
        std::to_string(g.generation)+":"+std::to_string(g.expires)+":"+g.block.hex();
    return {{"grant",wire::grant(g)},{"token",token},{"mode",manifest.mode==Mode::XPoint?"xpoint":"bsgs"},
        {"configuration",hex(inputs.blob(0))},{"targets",hex(inputs.blob(1))}};
}
Json Worker::status()const{
    auto& s=*impl_;Transaction tx(s.db,false);
    Statement q(s.db.handle(),"SELECT instance,client,last_ack,next_sync,schedule_boot,outbox_bytes,outbox_limit FROM worker_settings WHERE singleton=1");
    if(!q.step())throw std::runtime_error("worker is not configured");
    Json out{{"instance",q.text(0)},{"client",q.text(1)},{"last_ack_server_time",q.integer(2)},
        {"sync_due_in",q.text(4)==boot_id()?std::max(int64_t(0),q.integer(3)-s.timer()):0},
        {"outbox_bytes",q.integer(5)},{"outbox_limit",q.integer(6)},{"queues",Json::array()}};
    Statement denied(s.db.handle(),"SELECT value FROM metadata WHERE key='worker_sync_denial'");
    if(denied.step()){const auto reason=denied.blob(0);out["pause_reason"]=std::string(reason.begin(),reason.end());}
    Statement pending(s.db.handle(),"SELECT count(*) FROM worker_requests WHERE acknowledged=0");pending.step();out["pending_request"]=pending.integer(0)!=0;
    Statement grants(s.db.handle(),"SELECT project,job,block,device,boot,deadline,paused,acknowledged,remote FROM worker_grants ORDER BY project,job,block");
    while(grants.step()){
        const Scope scope{grants.text(0),wire::digest(hex(grants.blob(1)))};const auto state=s.journal.block(scope,grants.wide(2));
        std::string activity=state.state=="finished"?(grants.integer(7)?"server-acknowledged":"local-complete-awaiting-sync"):
            (grants.text(4)!=boot_id()||grants.integer(5)<=s.timer()||state.expired)?"needs-revalidation":grants.integer(6)?"server-paused":state.started?"started":"queued";
        out["queues"].push_back({{"project",scope.project},{"job",hex(bytes(scope.job))},{"block",grants.wide(2).hex()},
            {"device",grants.text(3)},{"activity",activity},{"local_state",state.state},
            {"server_expires",parse_json(grants.text(8))["expires"]}});
    }
    tx.commit();return out;
}
}
