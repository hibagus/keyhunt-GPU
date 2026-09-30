#include "keyhunt/coordinator/repository.h"
#include "protocol.h"
#include "keyhunt/storage/checkpoint.h"
#include <sstream>
namespace keyhunt::coordination {
using namespace storage;
using namespace storage::detail;
using namespace wire;
namespace {
struct Actor{std::string client;Bytes fingerprint;};
int role(const std::string& value){
    if(value=="reader")return 1;if(value=="worker")return 2;if(value=="owner")return 3;
    if(value=="none")return 0;throw Error(400,"role must be reader, worker, owner or none");
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
        if(!q.step())throw Error(404,"not found");const int r=int(q.integer(0));
        if(r<minimum)throw Error(403,"operation requires a higher project role");return r;
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
        for(const auto& v:b.covered)covered.push_back(interval(v));for(const auto& v:b.remaining)remaining.push_back(interval(v));
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
int64_t Repository::now()const{return impl_->now();}
std::string Repository::directory()const{return impl_->journal.state_directory();}
Json Repository::admin(const Json& body){
    auto& s=*impl_;const auto op=str(body,"operation",64);
    if(op=="check"){fields(body,{"operation"});s.journal.check();return {{"integrity","ok"},{"epoch",hex(s.db.metadata("epoch"))}};}
    if(op=="backup"){fields(body,{"operation","destination"});s.journal.check();s.journal.backup(str(body,"destination",4096));return {{"backed_up",true},{"quarantined",true}};}
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
        if(!sqlite3_changes(s.db.handle()))throw Error(404,"credential not found");result={{"updated",true}};
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
Json Repository::request(const Certificate& cert,const std::string& method,const std::string& path,const Json& body){
    auto& s=*impl_;const auto parts=split(path);Transaction tx(s.db,method!="GET");
    const auto actor=s.authenticate(cert);
    if(parts.size()<3||parts[0]!="api"||parts[1]!="v1")throw Error(404,"not found");
    Json out;
    if(parts==std::vector<std::string>{"api","v1","projects"}&&method=="GET"){
        out=Json::array();Statement q(s.db.handle(),"SELECT p.project,p.name,m.role FROM projects p JOIN coordinator_memberships m USING(project) WHERE m.client=? ORDER BY p.project");
        q.bind(1,actor.client);while(q.step())out.push_back({{"project",q.text(0)},{"name",q.text(1)},{"role",q.integer(2)}});
    }else{
        if(parts.size()<5||parts[2]!="projects")throw Error(404,"not found");
        const auto& project=parts[3];s.authorize(actor,project);
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
            }else if(parts.size()==7&&parts[6]=="results"&&method=="GET"){
                s.authorize(actor,project,3);out=Json::array();
                for(const auto& row:s.journal.results(scope))out.push_back({{"id",row.id},{"block",row.block.hex()},{"scalar",row.scalar.hex()},{"target",row.target},{"target_bytes",hex(row.target_bytes)}});
            }else throw Error(404,"not found");
        }else throw Error(404,"not found");
        if(method!="GET")s.event(actor.client,actor.fingerprint,path,project,"authorized mutation");
    }
    tx.commit();return out;
}
} // namespace keyhunt::coordination
