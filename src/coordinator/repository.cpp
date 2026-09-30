#include "keyhunt/coordinator/repository.h"
#include "protocol.h"
#include "keyhunt/storage/checkpoint.h"
#include <sstream>
#include <charconv>
#include <chrono>
namespace keyhunt::coordination {
using namespace storage;
using namespace storage::detail;
using namespace wire;
namespace {
struct Actor{std::string client;Bytes fingerprint;};
int role(const std::string& value){
    if(value=="reader")return 1;
    if(value=="worker")return 2;
    if(value=="owner")return 3;
    if(value=="none")return 0;
    throw Error(400,"role must be reader, worker, owner or none");
}
std::vector<std::string> split(const std::string& path){
    if(path.empty()||path[0]!='/'||path.find_first_of("%?#\\")!=std::string::npos)throw Error(404,"not found");
    std::vector<std::string> parts;std::istringstream input(path.substr(1));std::string part;
    while(std::getline(input,part,'/')){if(part.empty())throw Error(404,"not found");parts.push_back(part);}
    return parts;
}
}
struct Repository::Impl {
    Journal journal;Database& db;
    struct Budget {std::chrono::steady_clock::time_point since;unsigned count=0;};
    std::map<std::string,Budget> budgets;
    void rate(const std::string& key,unsigned limit){
        const auto time=std::chrono::steady_clock::now();
        for(auto it=budgets.begin();it!=budgets.end();)if(time-it->second.since>std::chrono::minutes(1))it=budgets.erase(it);else ++it;
        if(!budgets.count(key)&&budgets.size()>=16384)throw Error(429,"request budget table full");
        auto& budget=budgets.try_emplace(key,Budget{time,0}).first->second;
        if(++budget.count>limit)throw Error(429,"registered client/project request budget exceeded");
    }
    explicit Impl(const std::string& dir,Journal::Clock clock):journal(dir,std::move(clock)),db(journal.database()){}
    int64_t now()const{return journal.timestamp();}
    void event(const std::string& actor,const Bytes& credential,const std::string& operation,const std::string& project,const std::string& detail){
        Statement q(db.handle(),"INSERT INTO coordinator_events(created,actor,credential,operation,project,detail) VALUES(?,?,?,?,?,?)");
        q.bind(1,now());q.bind(2,actor);q.bind(3,credential);q.bind(4,operation);q.bind(5,project);q.bind(6,detail);q.step();
    }
    Actor authenticate(const Certificate& cert)const{
        if(cert.not_before>now()||cert.not_after<=now())throw Error(401,"credential unavailable");
        Statement q(db.handle(),"SELECT c.client,c.enabled,p.enabled,c.certificate,c.not_before,c.not_after FROM coordinator_credentials c JOIN coordinator_clients p USING(client) WHERE c.fingerprint=?");
        q.bind(1,cert.fingerprint);
        if(!q.step()||!q.integer(1)||!q.integer(2)||q.blob(3)!=cert.der||q.integer(4)!=cert.not_before||q.integer(5)!=cert.not_after)
            throw Error(401,"credential unavailable");
        return {q.text(0),cert.fingerprint};
    }
    int authorize(const Actor& actor,const std::string& project,int minimum=1)const{
        Statement q(db.handle(),"SELECT role FROM coordinator_memberships WHERE project=? AND client=?");q.bind(1,project);q.bind(2,actor.client);
        if(!q.step())throw Error(404,"not found");
        const int r=int(q.integer(0));
        if(r<minimum)throw Error(403,"operation requires a higher project role");
        return r;
    }
    void client_exists(const std::string& client)const{
        Statement q(db.handle(),"SELECT 1 FROM coordinator_clients WHERE client=?");q.bind(1,client);if(!q.step())throw Error(404,"client not found");
    }
    void member(const std::string& project,const std::string& client,int value){
        client_exists(client);
        if(value){Statement q(db.handle(),"INSERT INTO coordinator_memberships VALUES(?,?,?) ON CONFLICT(project,client) DO UPDATE SET role=excluded.role");q.bind(1,project);q.bind(2,client);q.bind(3,int64_t(value));q.step();}
        else{Statement q(db.handle(),"DELETE FROM coordinator_memberships WHERE project=? AND client=?");q.bind(1,project);q.bind(2,client);q.step();}
    }
    void enroll(const std::string& client,const Certificate& cert){
        if(cert.not_before>now()||cert.not_after<=now())throw Error(400,"cannot enroll expired or not-yet-valid credential");
        Statement q(db.handle(),"INSERT INTO coordinator_credentials VALUES(?,?,?,?,?,?,?,?,1)");
        q.bind(1,cert.fingerprint);q.bind(2,client);q.bind(3,cert.spki);q.bind(4,cert.issuer);q.bind(5,cert.serial);
        q.bind(6,cert.not_before);q.bind(7,cert.not_after);q.bind(8,cert.der);q.step();
    }
    Json block(const Scope& scope,const UInt256& id){
        const auto b=journal.block(scope,id);Json covered=Json::array(),remaining=Json::array();
        for(const auto& v:b.covered)covered.push_back(interval(v));
        for(const auto& v:b.remaining)remaining.push_back(interval(v));
        return {{"state",b.state},{"started",b.started},{"expired",b.expired},
            {"assignment",b.assignment?wire::grant(*b.assignment):Json(nullptr)},{"covered",covered},{"remaining",remaining}};
    }
    Json job(const Scope& scope){
        const auto m=journal.manifest(scope);auto result=wire::manifest(scope,m);const auto stats=journal.statistics(scope);
        result["blocks"]=stats.blocks.hex();result["unexplored"]=stats.unexplored.hex();result["finished"]=stats.finished.hex();
        result["assignments"]=stats.assignments;result["quarantined"]=stats.quarantined;
        Statement q(db.handle(),"SELECT paused FROM coordinator_controls WHERE project=? AND job=?");bind_scope(q,scope);
        result["paused"]=q.step()&&q.integer(0)!=0;return result;
    }
    Json sync(const Actor& actor, const Json& body) {
        fields(body, {"protocol", "capabilities", "instance", "request", "jobs", "updates", "returns"});
        if (integer(body, "protocol") != 1 || body["capabilities"] != Json({"checkpoint-v1", "offline-lease-v1"}))
            throw Error(426, "protocol 1 and checkpoint-v1/offline-lease-v1 capabilities required");
        const auto instance = str(body, "instance", 36), request = str(body, "request", 64);
        token(instance); token(request);
        const auto owner = actor.client + "." + instance;
        for (const auto* name : {"jobs", "updates", "returns"})
            if (!body[name].is_array() || body[name].size() > 128) throw Error(400, "invalid sync collection");
        if (body["jobs"].empty() || body["jobs"].size() > 64) throw Error(400, "sync needs 1..64 jobs");

        std::set<std::pair<std::string, Digest>> scopes;
        std::set<std::string> devices;
        for (const auto& row : body["jobs"]) {
            fields(row, {"project", "job", "devices", "spares", "policy"});
            const auto scope = wire::scope(row);
            authorize(actor, scope.project, 2);
            rate(actor.client+"/"+scope.project,120);
            journal.manifest(scope);
            if (!scopes.emplace(scope.project, scope.job).second) throw Error(400, "duplicate sync job");
            integer(row, "spares", 0, 1);
            const auto policy = str(row, "policy", 16);
            if (policy != "sequential" && policy != "random" && policy != "random-window")
                throw Error(400, "unsupported queue policy");
            if (!row["devices"].is_array() || row["devices"].size() > 64) throw Error(400, "invalid devices");
            for (const auto& device : row["devices"]) {
                if (!device.is_string()) throw Error(400, "invalid device token");
                const auto name = device.get<std::string>(); token(name);
                if (!devices.insert(name).second || devices.size() > 64) throw Error(400, "device assigned to multiple jobs");
            }
        }
        auto owned = [&](const Json& row) {
            auto g = wire::grant(row);
            if (!scopes.count({g.scope.project, g.scope.job})) throw Error(400, "grant scope missing from sync jobs");
            if (g.owner != owner) throw Error(403, "grant belongs to another machine");
            return g;
        };
        // Check the authorization of every scope before looking up a cached
        // receipt. Revoked membership must not disclose an old response.
        for (const auto& row : body["updates"]) {
            fields(row, {"grant", "started", "checkpoints"}); owned(row["grant"]);
            boolean(row, "started");
            if (!row["checkpoints"].is_array()) throw Error(400, "invalid checkpoint page");
        }
        for (const auto& row : body["returns"]) owned(row);
        const auto text = body.dump();
        if (text.size() > 8 * 1024 * 1024) throw Error(413, "sync request exceeds bounded page");
        const Bytes payload(text.begin(), text.end());
        Statement prior(db.handle(), "SELECT payload,response,epoch FROM coordinator_syncs WHERE client=? AND instance=? AND request=?");
        prior.bind(1, actor.client); prior.bind(2, instance); prior.bind(3, request);
        if (prior.step()) {
            if (prior.blob(0) != storage::detail::digest(payload)) throw Error(409, "sync key reused with changed payload");
            if (prior.blob(2) != db.metadata("epoch")) throw Error(409, "sync receipt belongs to an old coordinator epoch");
            return parse_json(prior.text(1));
        }
        Json accepted = Json::array(); size_t page_count = 0, match_count = 0, ordinal = 0;
        std::set<std::string> changed;
        auto unique_grant = [&](const Grant& g) {
            const auto key = g.scope.project + hex(bytes(g.scope.job)) + g.block.hex();
            if (!changed.insert(key).second) throw Error(400, "grant repeated in sync mutations");
        };
        for (const auto& row : body["updates"]) {
            const auto g = owned(row["grant"]); unique_grant(g);
            const auto state = journal.block(g.scope, g.block);
            if (!state.assignment || state.assignment->generation != g.generation || state.assignment->owner != owner ||
                g.epoch != db.metadata("epoch") || state.assignment->interval.begin() != g.interval.begin() ||
                state.assignment->interval.end() != g.interval.end()) throw Error(409, "stale assignment");
            if (state.expired) throw Error(409, "assignment expired; explicit recovery required");
            if (!row["checkpoints"].empty() && !boolean(row, "started")) throw Error(400, "checkpoints require started activity");
            Statement binding(db.handle(), "SELECT configuration,targets FROM search_bindings WHERE project=? AND job=?");
            bind_scope(binding, g.scope); if (!binding.step()) throw Error(409, "job lacks canonical search inputs");
            const auto input = decode_binding(journal.manifest(g.scope), binding.blob(0), binding.blob(1));
            core::XPointVerifier verifier;
            std::vector<CheckpointData> pages;
            for (const auto& encoded : row["checkpoints"]) {
                if (++page_count > 64 || !encoded.is_string()) throw Error(413, "too many checkpoint pages");
                auto page = decode_checkpoint(unhex(encoded.get<std::string>(), 2 * 1024 * 1024));
                if (page.block != g.block || page.generation != uint64_t(g.generation) || page.epoch != g.epoch)
                    throw Error(409, "checkpoint assignment mismatch");
                match_count += page.matches.size();
                if (match_count > 4096 || page.coverage.size() > 1024) throw Error(413, "checkpoint page exceeds work budget");
                for (const auto& interval : page.coverage)
                    if (!g.interval.contains(interval)) throw Error(400, "coverage outside assignment");
                for (const auto& match : page.matches) {
                    if (!g.interval.contains(match.scalar)) throw Error(400, "match outside assignment");
                    input.verify(verifier, match.scalar, match.target);
                }
                pages.push_back(std::move(page));
            }
            // The existing checkpoint journal retains canonical payloads and
            // audit receipts. Its savepoints remain inside this machine commit.
            int64_t executor = 0;
            if (boolean(row, "started")) executor = journal.begin_search(g);
            for (const auto& page : pages)
                journal.commit_search(g, executor, page.coverage, page.matches, request + ".c" + std::to_string(ordinal++));
            const bool complete = journal.block(g.scope, g.block).state == "finished";
            if (!complete) journal.renew(g, request + ".r" + std::to_string(ordinal++));
            accepted.push_back({{"project", g.scope.project}, {"job", hex(bytes(g.scope.job))},
                                {"block", g.block.hex()}, {"complete", complete}});
        }
        for (const auto& row : body["returns"]) {
            const auto g = owned(row); unique_grant(g);
            journal.return_unstarted(g, request + ".u" + std::to_string(ordinal++));
        }
        Json jobs = Json::array(), grants = Json::array();
        for (const auto& row : body["jobs"]) {
            const auto scope = wire::scope(row); auto info = job(scope);
            Statement binding(db.handle(), "SELECT configuration,targets FROM search_bindings WHERE project=? AND job=?");
            bind_scope(binding, scope); if (!binding.step()) throw Error(409, "job lacks canonical search inputs");
            info["configuration"] = hex(binding.blob(0)); info["targets"] = hex(binding.blob(1)); jobs.push_back(info);
            if (info["paused"].get<bool>()) continue;
            for (const auto& device : row["devices"]) {
                const auto name = device.get<std::string>();
                Statement count(db.handle(), "SELECT count(*) FROM coordinator_devices WHERE client=? AND instance=? AND device=?");
                count.bind(1, actor.client); count.bind(2, instance); count.bind(3, name); count.step();
                const int64_t needed = 1 + integer(row, "spares", 0, 1) - count.integer(0);
                if (needed <= 0) continue;
                Selection selection; selection.count = uint32_t(needed);
                const auto policy = str(row, "policy");
                selection.policy = policy == "random" ? Policy::Random : policy == "random-window" ? Policy::RandomWindow : Policy::Sequential;
                for (const auto& g : journal.claim(scope, owner, request + ".q" + std::to_string(ordinal++), selection)) {
                    Statement add(db.handle(), "INSERT INTO coordinator_devices VALUES(?,?,?,?,?,?)");
                    bind_scope(add, scope); add.bind(3, g.block); add.bind(4, actor.client); add.bind(5, instance); add.bind(6, name); add.step();
                }
            }
        }
        // Return only this machine's grants within the authorized request scope.
        // Old device queues still count toward the cap until explicitly returned.
        Statement assigned(db.handle(), "SELECT project,job,block,device FROM coordinator_devices WHERE client=? AND instance=? ORDER BY project,job,device,block");
        assigned.bind(1, actor.client); assigned.bind(2, instance);
        while (assigned.step()) {
            const Scope scope{assigned.text(0), wire::digest(hex(assigned.blob(1)))};
            if (!scopes.count({scope.project, scope.job})) continue;
            const auto state = journal.block(scope, assigned.wide(2));
            if (!state.assignment) throw std::runtime_error("device mapping lost assignment");
            Json covered=Json::array();
            if(state.covered.size()>1024)throw Error(413,"recovery coverage requires a smaller reconciled page");
            for(const auto& interval:state.covered)covered.push_back(wire::interval(interval));
            grants.push_back({{"device", assigned.text(3)}, {"grant", wire::grant(*state.assignment)}, {"covered",covered}});
            if (grants.size() > 128) throw Error(409, "machine queue exceeds 128 assignments");
        }
        // Report the final transaction state, including newly allocated queues.
        for (auto& info : jobs) {
            const auto current = job(wire::scope(info));
            for (const auto* key : {"unexplored", "finished", "assignments"}) info[key] = current[key];
        }
        Json result{{"protocol", 1}, {"client", actor.client}, {"instance", instance}, {"request", request},
                    {"epoch", hex(db.metadata("epoch"))}, {"issued_at", now()}, {"sync_seconds", 7200},
                    {"lifetime_seconds", 2592000}, {"accepted", accepted}, {"jobs", jobs}, {"grants", grants}};
        const auto response = result.dump();
        if (response.size() > 7 * 1024 * 1024) throw Error(413, "sync response exceeds bounded page");
        Statement insert(db.handle(), "INSERT INTO coordinator_syncs VALUES(?,?,?,?,?,?,?)");
        insert.bind(1, actor.client); insert.bind(2, instance); insert.bind(3, request);
        insert.bind(4, storage::detail::digest(payload)); insert.bind(5, response); insert.bind(6, db.metadata("epoch")); insert.bind(7, now()); insert.step();
        Statement issued(db.handle(), "UPDATE coordinator_settings SET last_issued=MAX(last_issued,?) WHERE singleton=1");
        issued.bind(1, now()); issued.step();
        event(actor.client, actor.fingerprint, "sync", "", Json({{"instance", instance}, {"request", request}, {"pages", page_count}}).dump());
        return result;
    }
    Json create_job(const std::string& project,const Json& body){
        fields(body,{"mode","begin","end_exclusive","block_width","configuration","targets"});
        const auto configuration=unhex(str(body,"configuration",100),50),targets=unhex(str(body,"targets",4*1024*1024));
        const auto mode=str(body,"mode",8);if(mode!="xpoint"&&mode!="bsgs")throw Error(400,"unknown search mode");
        Manifest m{mode=="xpoint"?Mode::XPoint:Mode::Bsgs,
            ScalarInterval(wide(str(body,"begin",66)),wide(str(body,"end_exclusive",66))),wide(str(body,"block_width",66)),{},{}};
        // Bind canonical targets without loading a resident GPU BSGS table. The
        // immutable configuration contains its semantic version, m and checksum.
        m.algorithm=wire::digest(wire::hex(storage::detail::digest(configuration)));
        if(mode=="xpoint"){
            if(targets.size()%32)throw Error(400,"invalid xpoint target bytes");
            std::vector<core::XPointBytes> values(targets.size()/32);
            for(size_t i=0;i<values.size();++i)std::copy_n(targets.begin()+32*i,32,values[i].begin());
            m.targets=core::XPointTargets(std::move(values)).digest();
        }else{
            if(targets.size()%65)throw Error(400,"invalid BSGS target bytes");
            std::vector<core::UncompressedPublicKey> values(targets.size()/65);
            for(size_t i=0;i<values.size();++i)std::copy_n(targets.begin()+65*i,65,values[i].begin());
            m.targets=core::BsgsPublicKeyTargets(std::move(values)).digest();
        }
        const auto input=decode_binding(m,configuration,targets);
        const auto scope=journal.create_job(project,m);journal.bind_search(scope,input);return job(scope);
    }
};
Repository::Repository(const std::string& path,Journal::Clock clock):impl_(std::make_unique<Impl>(path,std::move(clock))){}
Repository::~Repository()=default;
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
void Repository::test_page_limit(bool constrained){
    auto& db=impl_->db;Statement count(db.handle(),"PRAGMA page_count");count.step();
    db.exec("PRAGMA max_page_count="+std::to_string(constrained?count.integer(0):4294967294LL));
}
#endif
int64_t Repository::now()const{return impl_->now();}
std::string Repository::directory()const{return impl_->journal.state_directory();}
Json Repository::admin(const Json& body){
    auto& s=*impl_;const auto op=str(body,"operation",64);
    if(op=="check"){fields(body,{"operation"});s.journal.check();return {{"integrity","ok"},{"epoch",hex(s.db.metadata("epoch"))},{"quarantined",s.db.metadata("quarantine")!=Bytes{0}}};}
    if(op=="backup"){fields(body,{"operation","destination"});s.journal.check();s.journal.backup(str(body,"destination",4096));return {{"backed_up",true},{"quarantined",true}};}
    if(op=="activate-restore"){
        fields(body,{"operation","old_authority_stopped","all_previous_executors_stopped","access_review_complete"});
        if(!boolean(body,"old_authority_stopped")||!boolean(body,"all_previous_executors_stopped")||!boolean(body,"access_review_complete"))
            throw Error(409,"restore activation requires stopped authority/executors and completed access review");
        s.journal.check();Transaction tx(s.db);
        if(s.db.metadata("quarantine")!=Bytes{1})throw Error(409,"journal is not a quarantined restore");
        Statement worker(s.db.handle(),"SELECT count(*) FROM worker_settings");worker.step();
        if(worker.integer(0))throw Error(409,"worker snapshots need worker reconciliation, not coordinator activation");
        // Old snapshots can resurrect credentials revoked after their backup.
        // Disable every restored credential/client before reopening allocation;
        // the local operator must explicitly enroll/enable reviewed identities.
        s.db.exec("UPDATE coordinator_credentials SET enabled=0; UPDATE coordinator_clients SET enabled=0");
        s.db.metadata("quarantine",Bytes{0});
        s.event("local-admin",{},op,"","all prior executors stopped; restored access disabled");
        tx.commit();return {{"activated",true},{"restored_access_disabled",true},{"epoch",hex(s.db.metadata("epoch"))}};
    }
    Transaction tx(s.db);s.db.writable();Json result;
    if(op=="bootstrap"||op=="client-add"){
        fields(body,{"operation","name","certificate"});
        Statement boot(s.db.handle(),"SELECT bootstrapped FROM coordinator_settings WHERE singleton=1");boot.step();
        if(op=="bootstrap"?boot.integer(0)!=0:boot.integer(0)!=1)throw Error(409,"explicit first-operator bootstrap required exactly once");
        const auto cert=certificate(str(body,"certificate",16384));const auto client=uuid();
        Statement q(s.db.handle(),"INSERT INTO coordinator_clients VALUES(?,?,1)");q.bind(1,client);q.bind(2,str(body,"name"));q.step();
        s.enroll(client,cert);s.db.exec("UPDATE coordinator_settings SET bootstrapped=1 WHERE singleton=1");
        result={{"client",client},{"fingerprint",hex(cert.fingerprint)},{"spki",hex(cert.spki)},{"expires",cert.not_after}};
    }else if(op=="credential-add"){
        fields(body,{"operation","client","certificate"});const auto client=str(body,"client",36);s.client_exists(client);
        const auto cert=certificate(str(body,"certificate",16384));s.enroll(client,cert);result={{"client",client},{"fingerprint",hex(cert.fingerprint)}};
    }else if(op=="credential-set"){
        fields(body,{"operation","fingerprint","enabled"});const auto fp=bytes(wire::digest(str(body,"fingerprint",64)));
        Statement q(s.db.handle(),"UPDATE coordinator_credentials SET enabled=? WHERE fingerprint=?");q.bind(1,int64_t(boolean(body,"enabled")));q.bind(2,fp);q.step();
        if(!sqlite3_changes(s.db.handle()))throw Error(404,"credential not found");
        result={{"updated",true}};
    }else if(op=="client-set"){
        fields(body,{"operation","client","enabled"});const auto client=str(body,"client",36);s.client_exists(client);
        Statement q(s.db.handle(),"UPDATE coordinator_clients SET enabled=? WHERE client=?");q.bind(1,int64_t(boolean(body,"enabled")));q.bind(2,client);q.step();result={{"updated",true}};
    }else if(op=="project-create"){
        fields(body,{"operation","name","owner"});const auto owner=str(body,"owner",36);s.client_exists(owner);
        const auto project=s.journal.create_project(str(body,"name"));s.member(project,owner,3);result={{"project",project}};
    }else if(op=="membership-set"){
        fields(body,{"operation","project","client","role"});s.member(str(body,"project",36),str(body,"client",36),role(str(body,"role",8)));result={{"updated",true}};
    }else if(op=="clients"){
        fields(body,{"operation"});result=Json::array();Statement q(s.db.handle(),"SELECT c.client,c.name,c.enabled,k.fingerprint,k.spki,k.issuer,k.serial,k.not_before,k.not_after,k.enabled FROM coordinator_clients c JOIN coordinator_credentials k USING(client) ORDER BY c.client,k.fingerprint");
        while(q.step())result.push_back({{"client",q.text(0)},{"name",q.text(1)},{"client_enabled",bool(q.integer(2))},{"fingerprint",hex(q.blob(3))},
            {"spki",hex(q.blob(4))},{"issuer",q.text(5)},{"serial",q.text(6)},{"not_before",q.integer(7)},{"not_after",q.integer(8)},{"credential_enabled",bool(q.integer(9))}});
    }else throw Error(404,"unknown local administration operation");
    // Audit identifiers and actions only: never private keys or result bodies.
    s.event("local-admin",{},op,body.value("project",std::string()),result.dump());tx.commit();return result;
}
Json Repository::control_snapshot(const Certificate& cert,const Json& body){
    auto& s=*impl_;Transaction tx(s.db,false);
    if(s.db.metadata("quarantine")!=Bytes{0})throw Error(503,"coordinator restore is quarantined");
    const auto actor=s.authenticate(cert);Json controls=Json::array();
    for(const auto& row:body.at("jobs")){
        const auto scope=wire::scope(row);s.authorize(actor,scope.project,2);
        Statement q(s.db.handle(),"SELECT paused FROM coordinator_controls WHERE project=? AND job=?");bind_scope(q,scope);
        controls.push_back({{"project",scope.project},{"job",hex(bytes(scope.job))},{"paused",q.step()&&q.integer(0)!=0}});
    }
    tx.commit();return controls;
}
Json Repository::request(const Certificate& cert,const std::string& method,const std::string& path,const Json& body){
    auto& s=*impl_;const auto parts=split(path);Transaction tx(s.db,method!="GET");
    if(s.db.metadata("quarantine")!=Bytes{0})throw Error(503,"coordinator restore is quarantined");
    const auto actor=s.authenticate(cert);
    s.rate(actor.client,240);
    if(parts.size()<3||parts[0]!="api"||parts[1]!="v1")throw Error(404,"not found");
    Json out;
    if(parts==std::vector<std::string>{"api","v1","projects"}&&method=="GET"){
        out=Json::array();Statement q(s.db.handle(),"SELECT p.project,p.name,m.role FROM projects p JOIN coordinator_memberships m USING(project) WHERE m.client=? ORDER BY p.project");
        q.bind(1,actor.client);while(q.step())out.push_back({{"project",q.text(0)},{"name",q.text(1)},{"role",q.integer(2)}});
    }else if(parts==std::vector<std::string>{"api","v1","sync"}&&method=="POST"){
        s.db.writable();out=s.sync(actor,body);
    }else{
        if(parts.size()<5||parts[2]!="projects")throw Error(404,"not found");
        const auto& project=parts[3];s.authorize(actor,project);s.rate(actor.client+"/"+project,120);
        if(parts.size()==5&&parts[4]=="memberships"&&method=="POST"){
            s.db.writable();s.authorize(actor,project,3);fields(body,{"client","role"});
            s.member(project,str(body,"client",36),role(str(body,"role",8)));out={{"updated",true}};
        }else if(parts.size()==5&&parts[4]=="jobs"&&method=="POST"){
            s.db.writable();s.authorize(actor,project,3);out=s.create_job(project,body);
        }else if(parts.size()>=6&&parts[4]=="jobs"){
            Scope scope{project,wire::digest(parts[5])};
            if(parts.size()==7&&parts[6]=="status"&&method=="GET")out=s.job(scope);
            else if(parts.size()==8&&parts[6]=="blocks"&&method=="GET")out=s.block(scope,wide(parts[7]));
            else if(parts.size()==7&&parts[6]=="pause"&&method=="POST"){
                s.db.writable();s.authorize(actor,project,3);fields(body,{"paused"});s.journal.manifest(scope);
                Statement q(s.db.handle(),"INSERT INTO coordinator_controls VALUES(?,?,?) ON CONFLICT(project,job) DO UPDATE SET paused=excluded.paused");
                bind_scope(q,scope);q.bind(3,int64_t(boolean(body,"paused")));q.step();out={{"paused",body["paused"]}};
            }else if(parts.size()==9&&parts[6]=="blocks"&&parts[8]=="recover"&&method=="POST"){
                s.db.writable();s.authorize(actor,project,3);
                fields(body,{"client","instance","device","request","previous_executor_stopped"});
                if(!boolean(body,"previous_executor_stopped"))throw Error(409,"confirm the previous executor stopped before transfer");
                const auto client=str(body,"client",36),instance=str(body,"instance",36),device=str(body,"device",128),request=str(body,"request",64);
                token(instance);token(device);token(request);s.client_exists(client);
                s.authorize(Actor{client,{}},project,2);
                Statement enabled(s.db.handle(),"SELECT enabled FROM coordinator_clients WHERE client=?");enabled.bind(1,client);enabled.step();
                if(!enabled.integer(0))throw Error(409,"recovery client is disabled");
                const auto grant=s.journal.recover(scope,wide(parts[7]),client+"."+instance,request,true);
                const auto current=s.journal.block(scope,grant.block);
                if(!current.assignment||current.assignment->owner!=grant.owner||current.assignment->generation!=grant.generation)
                    throw Error(409,"recovery receipt has since been superseded");
                Statement mapping(s.db.handle(),"INSERT INTO coordinator_devices VALUES(?,?,?,?,?,?) ON CONFLICT(project,job,block) DO UPDATE SET client=excluded.client,instance=excluded.instance,device=excluded.device");
                bind_scope(mapping,scope);mapping.bind(3,grant.block);mapping.bind(4,client);mapping.bind(5,instance);mapping.bind(6,device);mapping.step();
                out={{"grant",wire::grant(grant)},{"block",s.block(scope,grant.block)}};
            }else if((parts.size()==7||parts.size()==9)&&parts[6]=="results"&&method=="GET"){
                s.authorize(actor,project,3);out=Json::array();
                int64_t after=0;uint32_t limit=100;
                if(parts.size()==9){
                    auto decimal=[](const std::string& text,int64_t maximum){int64_t n=0;const auto result=std::from_chars(text.data(),text.data()+text.size(),n);
                        if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size()||n<0||n>maximum||std::to_string(n)!=text)throw Error(400,"invalid result page");
                        return n;};
                    after=decimal(parts[7],INT64_MAX);limit=uint32_t(decimal(parts[8],1000));if(!limit)throw Error(400,"empty result page limit");
                }
                for(const auto& row:s.journal.results(scope,after,limit))out.push_back({{"id",row.id},{"block",row.block.hex()},{"scalar",row.scalar.hex()},{"target",row.target},{"target_bytes",hex(row.target_bytes)}});
            }else throw Error(404,"not found");
        }else throw Error(404,"not found");
        if(method!="GET")s.event(actor.client,actor.fingerprint,path,project,"authorized mutation");
    }
    tx.commit();return out;
}
} // namespace keyhunt::coordination
