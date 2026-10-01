#include "keyhunt/storage/journal.h"
#include "sqlite.h"
#include "free_tree.h"
#include "checkpoint_data.h"
#include <map>
#include <set>
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace keyhunt::storage {
const char* mode_name(Mode mode) {
    switch(mode){case Mode::XPoint:return "xpoint";case Mode::Bsgs:return "bsgs";case Mode::Hash160:return "hash160";case Mode::Ethereum:return "ethereum";case Mode::Vanity:return "vanity";case Mode::Minikeys:return "minikeys";}
    throw std::invalid_argument("unsupported journal search semantics");
}
size_t target_width(Mode mode) {
    switch(mode){case Mode::XPoint:return 32;case Mode::Bsgs:return 65;case Mode::Hash160:return 21;case Mode::Ethereum:return 20;case Mode::Vanity:return 36;case Mode::Minikeys:return 22;}
    throw std::invalid_argument("unsupported journal target format");
}

using namespace detail;
namespace {
Bytes bytes(const Digest& d){return Bytes(d.begin(),d.end());}
Digest fixed(const Bytes& b){if(b.size()!=32)throw std::runtime_error("invalid digest width");Digest d{};std::copy(b.begin(),b.end(),d.begin());return d;}
void append(Bytes& to,const Bytes& from){to.insert(to.end(),from.begin(),from.end());}
void append(Bytes& to,const UInt256& value){auto b=value.bytes();to.insert(to.end(),b.begin(),b.end());}
void number(Bytes& to,uint64_t value){for(int i=7;i>=0;--i)to.push_back(uint8_t(value>>(8*i)));}
uint64_t number(const Bytes& from,size_t& offset){if(from.size()-offset<8)throw std::runtime_error("short receipt");uint64_t n=0;for(unsigned i=0;i<8;++i)n=(n<<8)|from[offset++];return n;}
void text(Bytes& to,const std::string& s){number(to,s.size());to.insert(to.end(),s.begin(),s.end());}
void token(const std::string& s){if(s.empty() || s.size()>128)throw std::invalid_argument("identity/request must have 1..128 ASCII token characters");for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='@'))throw std::invalid_argument("invalid identity/request token");}
void project_id(const std::string& p){if(p.size()!=36)throw std::invalid_argument("project must be a canonical UUID");for(size_t i=0;i<p.size();++i){if(i==8||i==13||i==18||i==23){if(p[i]!='-')throw std::invalid_argument("invalid project UUID");}else if(!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')))throw std::invalid_argument("invalid project UUID");}}
Bytes encode(const Manifest& m){
    if(m.mode!=Mode::XPoint && m.mode!=Mode::Bsgs && m.mode!=Mode::Hash160 && m.mode!=Mode::Ethereum && m.mode!=Mode::Vanity && m.mode!=Mode::Minikeys)throw std::invalid_argument("unsupported journal search semantics");
    scheduler::BlockGrid grid(m.root,m.block_width);
    Bytes b{'k','h','j','o','b',1,uint8_t(m.mode)};
    append(b,m.root.begin());append(b,m.root.end());append(b,m.block_width);append(b,bytes(m.targets));append(b,bytes(m.algorithm));return b;
}
Manifest decode(const Bytes& b){
    if(b.size()!=167 || !std::equal(b.begin(),b.begin()+6,Bytes{'k','h','j','o','b',1}.begin()))throw std::runtime_error("unknown canonical job manifest");
    auto wide=[&](size_t at){UInt256::Bytes n{};std::copy_n(b.begin()+at,32,n.begin());return UInt256::from_bytes(n);};
    Manifest m{Mode(b[6]),ScalarInterval(wide(7),wide(39)),wide(71),fixed(Bytes(b.begin()+103,b.begin()+135)),fixed(Bytes(b.begin()+135,b.end()))};
    if(encode(m)!=b)throw std::runtime_error("noncanonical job manifest");
    return m;
}
struct Job {Manifest manifest;scheduler::BlockGrid grid;Bytes seed;UInt256 counter;int64_t generation;};
void scope_bind(Statement& s,const Scope& scope){s.bind(1,scope.project);s.bind(2,bytes(scope.job));}
Bytes grant_payload(const Grant& g){Bytes b;append(b,g.block);text(b,g.owner);number(b,uint64_t(g.generation));number(b,uint64_t(g.expires));append(b,g.epoch);return b;}
}
struct Journal::Impl {
    mutable Database db;
    Clock clock;
    bool importing_remote=false;
    Impl(const std::string& path,Clock now):db(path),clock(std::move(now)){
        if(!clock)clock=[]{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();};
    }
    int64_t now()const{const auto n=clock();if(n<0)throw std::runtime_error("invalid journal clock");return n;}
    int64_t expiry(int64_t lifetime)const{if(lifetime<1 || lifetime>30*24*60*60)throw std::invalid_argument("assignment lifetime must be 1..2592000 seconds");const auto n=now();if(n>INT64_MAX-lifetime)throw std::overflow_error("assignment expiry overflow");return n+lifetime;}
    Job job(const Scope& scope)const{
        project_id(scope.project);
        Statement s(db.handle(),"SELECT manifest,root_begin,root_end,width,block_count,seed,counter,next_generation FROM jobs WHERE project=? AND job=?");scope_bind(s,scope);
        if(!s.step())throw std::invalid_argument("job is not in this project");
        const auto encoded=s.blob(0);auto m=decode(encoded);scheduler::BlockGrid grid(m.root,m.block_width);
        if(fixed(digest(encoded))!=scope.job || s.wide(1)!=m.root.begin() || s.wide(2)!=m.root.end() || s.wide(3)!=m.block_width || s.wide(4)!=grid.count())throw std::runtime_error("job identity/geometry mismatch");
        auto seed=s.blob(5);if(seed.size()!=32 || s.integer(7)<1)throw std::runtime_error("invalid scheduler state");
        return {m,grid,seed,s.wide(6),s.integer(7)};
    }
    void scheduler(const Scope& scope,const Job& job){Statement s(db.handle(),"UPDATE jobs SET counter=?,next_generation=? WHERE project=? AND job=?");s.bind(1,job.counter);s.bind(2,job.generation);s.bind(3,scope.project);s.bind(4,bytes(scope.job));s.step();}
    UInt256 random_below(Job& job,const UInt256& bound){
        if(bound.is_zero())throw std::invalid_argument("empty random domain");
        const auto max=bound.subtract(UInt256(1)).bytes();unsigned first=0;while(first<32 && !max[first])++first;
        if(first==32)return UInt256();
        unsigned bits=0;for(uint8_t x=max[first];x;x>>=1)++bits;
        // Draw a rank from only the required bits and reject the numeric tail.
        // This never guesses occupied block IDs and remains efficient at exhaustion.
        for(unsigned attempt=0;attempt<1024;++attempt){
            Bytes input{'k','h','r','n','g',1};append(input,job.seed);append(input,job.counter);
            job.counter=job.counter.add(UInt256(1));auto raw=digest(input);
            std::fill(raw.begin(),raw.begin()+first,0);raw[first]&=uint8_t((1U<<bits)-1);
            UInt256::Bytes b{};std::copy(raw.begin(),raw.end(),b.begin());auto value=UInt256::from_bytes(b);if(value<bound)return value;
        }
        throw std::runtime_error("bounded random sampler did not converge");
    }
    std::optional<Bytes> retry(const Scope& scope,const std::string& owner,const std::string& operation,const std::string& request,const Bytes& payload)const{
        token(owner);token(request);
        Statement s(db.handle(),"SELECT payload,response FROM requests WHERE project=? AND job=? AND owner=? AND operation=? AND request=?");
        scope_bind(s,scope);s.bind(3,owner);s.bind(4,operation);s.bind(5,request);
        if(!s.step())return {};
        if(s.blob(0)!=digest(payload))throw std::invalid_argument("idempotency key reused with different payload");
        return s.blob(1);
    }
    void receipt(const Scope& scope,const std::string& owner,const std::string& operation,const std::string& request,const Bytes& payload,const Bytes& response){
        Statement s(db.handle(),"INSERT INTO requests VALUES(?,?,?,?,?,?,?)");scope_bind(s,scope);s.bind(3,owner);s.bind(4,operation);s.bind(5,request);s.bind(6,digest(payload));s.bind(7,response);s.step();
        Statement e(db.handle(),"INSERT INTO events(project,job,operation,owner,request,created) VALUES(?,?,?,?,?,?)");scope_bind(e,scope);e.bind(3,operation);e.bind(4,owner);e.bind(5,request);e.bind(6,now());e.step();
    }
    Bytes response(const std::vector<Grant>& grants)const{
        Bytes b{1};append(b,db.metadata("epoch"));number(b,grants.size());
        for(const auto& g:grants){append(b,g.block);number(b,uint64_t(g.generation));number(b,uint64_t(g.expires));}return b;
    }
    std::vector<Grant> response(const Scope& scope,const std::string& owner,const Job& job,const Bytes& b)const{
        if(b.size()<25 || b[0]!=1)throw std::runtime_error("invalid assignment receipt");
        size_t offset=17;const auto count=number(b,offset);if(count>256 || b.size()!=25+48*count)throw std::runtime_error("invalid assignment receipt length");
        std::vector<Grant> result;const Bytes epoch(b.begin()+1,b.begin()+17);
        for(uint64_t i=0;i<count;++i){UInt256::Bytes raw{};std::copy_n(b.begin()+offset,32,raw.begin());offset+=32;auto block=UInt256::from_bytes(raw);auto gen=number(b,offset),expires=number(b,offset);if(!gen||gen>INT64_MAX||!expires||expires>INT64_MAX)throw std::runtime_error("invalid receipt generation/deadline");result.push_back({scope,block,job.grid.block(block),owner,int64_t(gen),int64_t(expires),epoch});}return result;
    }
    std::optional<Grant> assignment(const Scope& scope,const Job& job,const UInt256& id,bool* started=nullptr)const{
        const auto interval=job.grid.block(id);
        Statement s(db.handle(),"SELECT owner,generation,expires,started FROM assignments WHERE project=? AND job=? AND block=?");scope_bind(s,scope);s.bind(3,id);if(!s.step())return {};
        if(started)*started=s.integer(3)!=0;
        return Grant{scope,id,interval,s.text(0),s.integer(1),s.integer(2),db.metadata("epoch")};
    }
    Grant authorized(const Grant& g,const Job& job,bool* started=nullptr)const{
        if(g.epoch!=db.metadata("epoch"))throw ExecutionBlocked(ExecutionBlocked::Reason::Fence,"stale journal epoch");
        auto current=assignment(g.scope,job,g.block,started);
        if(!current || current->owner!=g.owner || current->generation!=g.generation || current->interval.begin()!=g.interval.begin() || current->interval.end()!=g.interval.end())throw ExecutionBlocked(ExecutionBlocked::Reason::Fence,"stale or foreign assignment");
        if(current->expires<=now())throw ExecutionBlocked(ExecutionBlocked::Reason::Expired,"assignment expired; explicit recovery required");
        if(remote(g.scope)&&!importing_remote){
            Statement q(db.handle(),"SELECT generation,boot,deadline,paused FROM worker_grants WHERE project=? AND job=? AND block=?");
            scope_bind(q,g.scope);q.bind(3,g.block);
            if(!q.step()||q.integer(0)!=g.generation||q.text(1)!=boot_id()||q.integer(2)<=boot_seconds())
                throw ExecutionBlocked(ExecutionBlocked::Reason::Revalidation,"offline deadline uncertain or expired; synchronize before execution");
            if(q.integer(3))throw ExecutionBlocked(ExecutionBlocked::Reason::Paused,"coordinator paused this job");
        }
        return *current;
    }
    std::vector<ScalarInterval> coverage(const Scope& scope,const UInt256& id)const{
        Statement s(db.handle(),"SELECT begin,end FROM coverage WHERE project=? AND job=? AND block=? ORDER BY begin");scope_bind(s,scope);s.bind(3,id);std::vector<ScalarInterval> out;while(s.step())out.emplace_back(s.wide(0),s.wide(1));return out;
    }
    bool remote(const Scope& scope)const{
        Statement q(db.handle(),"SELECT 1 FROM worker_jobs WHERE project=? AND job=?");scope_bind(q,scope);return q.step();
    }
    void local_only(const Scope& scope)const{
        if(remote(scope))throw std::invalid_argument("remote jobs require coordinator allocation and lease control");
    }
    bool bound(const Scope& scope)const{
        Statement q(db.handle(),"SELECT 1 FROM search_bindings WHERE project=? AND job=?");scope_bind(q,scope);return q.step();
    }
    void executor(const Grant& grant,int64_t generation)const{
        Statement q(db.handle(),"SELECT assignment_generation,executor_generation FROM executors WHERE project=? AND job=? AND block=?");
        scope_bind(q,grant.scope);q.bind(3,grant.block);
        if(!q.step() || q.integer(0)!=grant.generation || q.integer(1)!=generation)
            throw ExecutionBlocked(ExecutionBlocked::Reason::Fence,"stale checkpoint executor");
    }
    void apply_coverage(const Grant& grant,const std::vector<ScalarInterval>& intervals){
        if(intervals.empty())return; // partial BSGS targets may commit matches alone
    auto all=coverage(grant.scope,grant.block);
    for(const auto& v:all)if(!grant.interval.contains(v))throw std::runtime_error("stored coverage escaped its block");
    for(const auto& v:intervals){if(!grant.interval.contains(v))throw std::invalid_argument("coverage outside assigned block");all.push_back(v);}
    std::sort(all.begin(),all.end(),[](const auto& a,const auto& b){return a.begin()<b.begin();});
    std::vector<ScalarInterval> merged;
    for(const auto& v:all){if(merged.empty() || merged.back().end()<v.begin())merged.push_back(v);else merged.back()=ScalarInterval(merged.back().begin(),std::max(merged.back().end(),v.end()));}
    Statement remove(db.handle(),"DELETE FROM coverage WHERE project=? AND job=? AND block=?");scope_bind(remove,grant.scope);remove.bind(3,grant.block);remove.step();
    if(merged.size()==1 && merged[0].begin()==grant.interval.begin() && merged[0].end()==grant.interval.end()){
        Statement done(db.handle(),"DELETE FROM assignments WHERE project=? AND job=? AND block=?");scope_bind(done,grant.scope);done.bind(3,grant.block);done.step();merge_finished(grant.scope,grant.block);
    }else for(const auto& v:merged){Statement q(db.handle(),"INSERT INTO coverage VALUES(?,?,?,?,?)");scope_bind(q,grant.scope);q.bind(3,grant.block);q.bind(4,v.begin());q.bind(5,v.end());q.step();}
    }
    void merge_finished(const Scope& scope,const UInt256& block){
        auto begin=block,end=block.add(UInt256(1));
        Statement s(db.handle(),"SELECT begin,end FROM finished WHERE project=? AND job=? AND end>=? AND begin<=? ORDER BY begin");scope_bind(s,scope);s.bind(3,begin);s.bind(4,end);
        while(s.step()){begin=std::min(begin,s.wide(0));end=std::max(end,s.wide(1));}
        Statement d(db.handle(),"DELETE FROM finished WHERE project=? AND job=? AND end>=? AND begin<=?");scope_bind(d,scope);d.bind(3,begin);d.bind(4,end);d.step();
        Statement insert(db.handle(),"INSERT INTO finished VALUES(?,?,?,?)");scope_bind(insert,scope);insert.bind(3,begin);insert.bind(4,end);insert.step();
    }
};
Journal::Journal(const std::string& directory,Clock clock):impl_(std::make_unique<Impl>(directory,std::move(clock))){}
Journal::~Journal()=default;
detail::Database& Journal::database()const{return impl_->db;}
int64_t Journal::timestamp()const{return impl_->now();}
std::string Journal::create_project(const std::string& name){
    if(name.empty()||name.size()>256)throw std::invalid_argument("project name must have 1..256 printable ASCII characters");
    for(unsigned char c:name)if(c<32||c>126)throw std::invalid_argument("project name must be printable ASCII");
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();auto id=uuid();Statement q(s.db.handle(),"INSERT INTO projects VALUES(?,?)");q.bind(1,id);q.bind(2,name);q.step();tx.commit();return id;
}
Scope Journal::create_job(const std::string& project,const Manifest& manifest,std::optional<Digest> seed){
    project_id(project);const auto encoded=encode(manifest);Scope scope{project,fixed(digest(encoded))};scheduler::BlockGrid grid(manifest.root,manifest.block_width);
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();
    Statement q(s.db.handle(),"INSERT INTO jobs VALUES(?,?,?,?,?,?,?,?,?,1) ON CONFLICT(project,job) DO NOTHING");scope_bind(q,scope);q.bind(3,encoded);q.bind(4,manifest.root.begin());q.bind(5,manifest.root.end());q.bind(6,manifest.block_width);q.bind(7,grid.count());q.bind(8,seed?bytes(*seed):random_bytes(32));q.bind(9,UInt256());q.step();
    (void)s.job(scope);tx.commit();return scope;
}
Manifest Journal::manifest(const Scope& scope)const{return impl_->job(scope).manifest;}
std::vector<Grant> Journal::claim(const Scope& scope,const std::string& owner,const std::string& request,Selection choice,int64_t lifetime){
    if(!choice.count||choice.count>256 || choice.window.is_zero())throw std::invalid_argument("claim count must be 1..256 and window positive");
    if(choice.policy!=Policy::Sequential && choice.policy!=Policy::Random && choice.policy!=Policy::RandomWindow && choice.policy!=Policy::Manual)throw std::invalid_argument("unknown selection policy");
    if((choice.policy==Policy::Manual)!=(bool(choice.block)) || (choice.block && choice.count!=1))throw std::invalid_argument("manual selection needs exactly one explicit block");
    Bytes payload;number(payload,uint64_t(choice.policy));number(payload,choice.count);append(payload,choice.block.value_or(UInt256()));append(payload,choice.window);number(payload,uint64_t(lifetime));
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();s.local_only(scope);auto job=s.job(scope);const auto expires=s.expiry(lifetime);
    if(auto old=s.retry(scope,owner,"claim",request,payload)){auto grants=s.response(scope,owner,job,*old);tx.commit();return grants;}
    FreeTree tree(s.db,scope,job.grid.count());UInt256 begin,end=job.grid.count();
    if(choice.block){job.grid.block(*choice.block);if(tree.count(*choice.block,choice.block->add(UInt256(1))).is_zero())throw std::invalid_argument("block is already in_progress or finished");}
    else if(choice.policy==Policy::RandomWindow && !tree.free().is_zero()){
        const auto anchor=tree.select(s.random_below(job,tree.free()));begin=anchor.divmod(choice.window).first.multiply(choice.window);end=begin.add(std::min(choice.window,job.grid.count().subtract(begin)));
    }
    std::vector<Grant> grants;
    for(uint32_t i=0;i<choice.count;++i){
        const auto free=tree.count(begin,end);if(free.is_zero())break;
        UInt256 id;
        if(choice.block)id=*choice.block;
        else if(choice.policy==Policy::Sequential)id=tree.select(UInt256());
        else id=tree.select(tree.count(UInt256(),begin).add(s.random_below(job,free)));
        if(job.generation==INT64_MAX)throw std::overflow_error("assignment generation exhausted");
        Grant grant{scope,id,job.grid.block(id),owner,job.generation++,expires,s.db.metadata("epoch")};
        tree.occupy(id);
        Statement insert(s.db.handle(),"INSERT INTO assignments VALUES(?,?,?,?,?,?,0)");scope_bind(insert,scope);insert.bind(3,id);insert.bind(4,owner);insert.bind(5,grant.generation);insert.bind(6,expires);insert.step();grants.push_back(grant);
    }
    s.scheduler(scope,job);s.receipt(scope,owner,"claim",request,payload,s.response(grants));tx.commit();return grants;
}
void Journal::start(const Grant& grant,const std::string& request){
    auto& s=*impl_;const auto payload=grant_payload(grant);Transaction tx(s.db);s.db.writable();auto job=s.job(grant.scope);
    if(s.retry(grant.scope,grant.owner,"start",request,payload)){tx.commit();return;}s.authorized(grant,job);
    Statement q(s.db.handle(),"UPDATE assignments SET started=1 WHERE project=? AND job=? AND block=?");scope_bind(q,grant.scope);q.bind(3,grant.block);q.step();
    s.receipt(grant.scope,grant.owner,"start",request,payload,{});tx.commit();
}
Grant Journal::renew(const Grant& grant,const std::string& request,int64_t lifetime){
    auto& s=*impl_;auto payload=grant_payload(grant);number(payload,uint64_t(lifetime));Transaction tx(s.db);s.db.writable();s.local_only(grant.scope);auto job=s.job(grant.scope);auto expires=s.expiry(lifetime);
    if(auto old=s.retry(grant.scope,grant.owner,"renew",request,payload)){auto result=s.response(grant.scope,grant.owner,job,*old).at(0);tx.commit();return result;}
    auto current=s.authorized(grant,job);current.expires=std::max(current.expires,expires);
    Statement q(s.db.handle(),"UPDATE assignments SET expires=? WHERE project=? AND job=? AND block=?");q.bind(1,current.expires);q.bind(2,grant.scope.project);q.bind(3,bytes(grant.scope.job));q.bind(4,grant.block);q.step();
    s.receipt(grant.scope,grant.owner,"renew",request,payload,s.response({current}));tx.commit();return current;
}
Grant Journal::recover(const Scope& scope,const UInt256& block,const std::string& owner,const std::string& request,bool stopped,int64_t lifetime){
    auto& s=*impl_;Bytes payload;append(payload,block);number(payload,stopped);number(payload,uint64_t(lifetime));Transaction tx(s.db);s.db.writable();s.local_only(scope);auto job=s.job(scope);const auto expires=s.expiry(lifetime);
    if(auto old=s.retry(scope,owner,"recover",request,payload)){auto result=s.response(scope,owner,job,*old).at(0);tx.commit();return result;}
    auto current=s.assignment(scope,job,block);if(!current)throw std::invalid_argument("recovery needs an in_progress block");
    if(current->expires>s.now() && !stopped)throw std::invalid_argument("confirm the previous executor stopped before live transfer");
    if(job.generation==INT64_MAX)throw std::overflow_error("assignment generation exhausted");
    current->owner=owner;current->generation=job.generation++;current->expires=expires;
    Statement q(s.db.handle(),"UPDATE assignments SET owner=?,generation=?,expires=? WHERE project=? AND job=? AND block=?");q.bind(1,owner);q.bind(2,current->generation);q.bind(3,expires);q.bind(4,scope.project);q.bind(5,bytes(scope.job));q.bind(6,block);q.step();
    Statement old(s.db.handle(),"DELETE FROM executors WHERE project=? AND job=? AND block=?");scope_bind(old,scope);old.bind(3,block);old.step();
    s.scheduler(scope,job);s.receipt(scope,owner,"recover",request,payload,s.response({*current}));tx.commit();return *current;
}
void Journal::return_unstarted(const Grant& grant,const std::string& request){
    auto& s=*impl_;const auto payload=grant_payload(grant);Transaction tx(s.db);s.db.writable();s.local_only(grant.scope);auto job=s.job(grant.scope);
    if(s.retry(grant.scope,grant.owner,"return",request,payload)){tx.commit();return;}
    bool started=false;s.authorized(grant,job,&started);
    if(started || !s.coverage(grant.scope,grant.block).empty())throw std::invalid_argument("only an unstarted spare may be returned");
    Statement d(s.db.handle(),"DELETE FROM assignments WHERE project=? AND job=? AND block=?");scope_bind(d,grant.scope);d.bind(3,grant.block);d.step();FreeTree(s.db,grant.scope,job.grid.count()).release(grant.block);
    s.receipt(grant.scope,grant.owner,"return",request,payload,{});tx.commit();
}
void Journal::record_coverage(const Grant& grant,const std::vector<ScalarInterval>& intervals,const std::string& request){
    if(intervals.empty() || intervals.size()>1024)throw std::invalid_argument("coverage batch must have 1..1024 intervals");
    auto& s=*impl_;auto payload=grant_payload(grant);for(const auto& v:intervals){append(payload,v.begin());append(payload,v.end());}
    Transaction tx(s.db);s.db.writable();auto job=s.job(grant.scope);if(s.bound(grant.scope))throw std::invalid_argument("bound search requires verified checkpoints");if(s.retry(grant.scope,grant.owner,"coverage",request,payload)){tx.commit();return;}
    bool started=false;s.authorized(grant,job,&started);if(!started)throw std::invalid_argument("coverage needs a started assignment");
    s.apply_coverage(grant,intervals);
    s.receipt(grant.scope,grant.owner,"coverage",request,payload,{});tx.commit();
}

Grant Journal::import_remote(const Grant& remote,const Manifest& manifest,const Binding& input,
    const std::string& encoded,const std::string& device,int64_t deadline,int64_t local_expiry,bool paused,const std::vector<ScalarInterval>& accepted){
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();
    project_id(remote.scope.project);token(remote.owner);token(device);
    if(remote.generation==INT64_MAX || fixed(digest(encode(manifest)))!=remote.scope.job ||
       deadline<0 || local_expiry<=0)throw std::invalid_argument("invalid remote assignment manifest/deadline");
    const scheduler::BlockGrid grid(manifest.root,manifest.block_width);
    const auto interval=grid.block(remote.block);
    if(interval.begin()!=remote.interval.begin()||interval.end()!=remote.interval.end())throw std::invalid_argument("remote assignment bounds changed");
    Statement exists(s.db.handle(),"SELECT 1 FROM jobs WHERE project=? AND job=?");scope_bind(exists,remote.scope);
    if(exists.step()&&!s.remote(remote.scope))throw std::invalid_argument("cannot mix standalone allocation with a remote job");
    Statement project(s.db.handle(),"INSERT INTO projects VALUES(?,'coordinator import') ON CONFLICT(project) DO NOTHING");project.bind(1,remote.scope.project);project.step();
    create_job(remote.scope.project,manifest);bind_search(remote.scope,input);
    Statement job(s.db.handle(),"INSERT INTO worker_jobs VALUES(?,?) ON CONFLICT DO NOTHING");scope_bind(job,remote.scope);job.step();
    Statement previous(s.db.handle(),"SELECT generation FROM worker_grants WHERE project=? AND job=? AND block=?");scope_bind(previous,remote.scope);previous.bind(3,remote.block);
    const bool new_import=!previous.step();
    if(!new_import&&previous.integer(0)!=remote.generation)
        throw std::invalid_argument("recovered generation needs explicit stopped-worker reconciliation");
    Grant local=remote;local.epoch=s.db.metadata("epoch");local.expires=local_expiry;
    const auto state=block(remote.scope,remote.block);
    if(state.state!="finished"){
        if(!state.assignment)FreeTree(s.db,remote.scope,grid.count()).occupy(remote.block);
        else if(state.assignment->generation!=remote.generation||state.assignment->owner!=remote.owner)
            throw std::invalid_argument("remote grant conflicts with local owner");
        Statement grant(s.db.handle(),"INSERT INTO assignments VALUES(?,?,?,?,?,?,0) ON CONFLICT(project,job,block) DO UPDATE SET expires=excluded.expires");
        scope_bind(grant,remote.scope);grant.bind(3,remote.block);grant.bind(4,remote.owner);grant.bind(5,remote.generation);grant.bind(6,local_expiry);grant.step();
    }
    Statement generation(s.db.handle(),"UPDATE jobs SET next_generation=MAX(next_generation,?) WHERE project=? AND job=?");
    generation.bind(1,remote.generation+1);generation.bind(2,remote.scope.project);generation.bind(3,bytes(remote.scope.job));generation.step();
    Statement mapping(s.db.handle(),"INSERT INTO worker_grants VALUES(?,?,?,?,?,?,?,?,?,0) ON CONFLICT(project,job,block) DO UPDATE SET remote=excluded.remote,device=excluded.device,boot=excluded.boot,deadline=excluded.deadline,paused=excluded.paused");
    scope_bind(mapping,remote.scope);mapping.bind(3,remote.block);mapping.bind(4,remote.generation);mapping.bind(5,encoded);mapping.bind(6,device);
    mapping.bind(7,boot_id());mapping.bind(8,deadline);mapping.bind(9,int64_t(paused));mapping.step();
    if(new_import&&!accepted.empty()&&local_expiry>s.now()&&deadline>boot_seconds()){
        // A fresh recovery destination imports the coordinator's already durable
        // coverage. Its local receipt is trusted server state, not new GPU work,
        // so it is not put back into the upload outbox. Older results stay on the
        // coordinator and remain accessible through its owner-only results API.
        struct ImportGuard {bool& value;explicit ImportGuard(bool& v):value(v){value=true;}~ImportGuard(){value=false;}} guard(s.importing_remote);
        const auto executor=begin_search(local);
        commit_search(local,executor,accepted,{},"import-"+uuid());
    }
    tx.commit();return local;
}
void Journal::bind_search(const Scope& scope,const Binding& binding){
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();const auto job=s.job(scope);
    if(job.manifest.mode!=binding.mode || job.manifest.targets!=binding.target_digest || job.manifest.algorithm!=binding.algorithm_digest)
        throw std::invalid_argument("resolved inputs do not match the registered job");
    Statement prior(s.db.handle(),"SELECT configuration,targets FROM search_bindings WHERE project=? AND job=?");scope_bind(prior,scope);
    if(prior.step()){
        if(prior.blob(0)!=binding.configuration || prior.blob(1)!=binding.targets)throw std::runtime_error("search binding changed");
    }else{
        // C12 accepted synthetic coverage without a verified result journal.
        // Never upgrade that coverage into proof that real targets were searched.
        Statement progress(s.db.handle(),"SELECT (SELECT count(*) FROM coverage WHERE project=? AND job=?)+(SELECT count(*) FROM finished WHERE project=? AND job=?)");
        scope_bind(progress,scope);progress.bind(3,scope.project);progress.bind(4,bytes(scope.job));progress.step();
        if(progress.integer(0))throw std::invalid_argument("unverified legacy coverage cannot become a checkpointed search");
        Statement add(s.db.handle(),"INSERT INTO search_bindings VALUES(?,?,?,?,'keyhunt-C13-v1',1)");
        scope_bind(add,scope);add.bind(3,binding.configuration);add.bind(4,binding.targets);add.step();
    }
    tx.commit();
}
int64_t Journal::begin_search(const Grant& grant){
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();const auto job=s.job(grant.scope);s.authorized(grant,job);
    Statement next(s.db.handle(),"SELECT next_executor FROM search_bindings WHERE project=? AND job=?");scope_bind(next,grant.scope);
    if(!next.step())throw std::invalid_argument("search is not bound to canonical inputs");
    const auto generation=next.integer(0);if(generation==INT64_MAX)throw std::overflow_error("executor generation exhausted");
    Statement update(s.db.handle(),"UPDATE search_bindings SET next_executor=? WHERE project=? AND job=?");
    update.bind(1,generation+1);update.bind(2,grant.scope.project);update.bind(3,bytes(grant.scope.job));update.step();
    Statement active(s.db.handle(),"INSERT INTO executors VALUES(?,?,?,?,?) ON CONFLICT(project,job,block) DO UPDATE SET assignment_generation=excluded.assignment_generation,executor_generation=excluded.executor_generation");
    scope_bind(active,grant.scope);active.bind(3,grant.block);active.bind(4,grant.generation);active.bind(5,generation);active.step();
    Statement start(s.db.handle(),"UPDATE assignments SET started=1 WHERE project=? AND job=? AND block=?");
    scope_bind(start,grant.scope);start.bind(3,grant.block);start.step();
    s.receipt(grant.scope,grant.owner,"execute",uuid(),grant_payload(grant),{});
    tx.commit();return generation;
}
void Journal::validate_search(const Grant& grant,int64_t executor)const{
    auto& s=*impl_;Transaction tx(s.db,false);s.db.writable();s.authorized(grant,s.job(grant.scope));s.executor(grant,executor);tx.commit();
}
void Journal::commit_search(const Grant& grant,int64_t executor,const std::vector<ScalarInterval>& coverage,
    const std::vector<core::XPointMatch>& matches,const std::string& request){
    auto& s=*impl_;
    const auto payload=encode_checkpoint({grant.block,uint64_t(grant.generation),uint64_t(executor),grant.epoch,coverage,matches});
    Transaction tx(s.db);s.db.writable();const auto job=s.job(grant.scope);
    if(s.retry(grant.scope,grant.owner,"checkpoint",request,payload)){tx.commit();return;}
    bool started=false;s.authorized(grant,job,&started);s.executor(grant,executor);
    if(!started)throw std::invalid_argument("checkpoint requires a started assignment");
    for(const auto& match:matches){
        if(!grant.interval.contains(match.scalar))throw std::invalid_argument("match outside assigned block");
        Statement q(s.db.handle(),"INSERT INTO results(project,job,block,scalar,target) VALUES(?,?,?,?,?) ON CONFLICT(project,job,scalar,target) DO NOTHING");
        scope_bind(q,grant.scope);q.bind(3,grant.block);q.bind(4,match.scalar);q.bind(5,int64_t(match.target));q.step();
    }
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("after_results");
#endif
    s.apply_coverage(grant,coverage);
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("after_coverage");
#endif
    // Match rows, accepted interval union and the replay receipt are one WAL
    // transaction. A lost acknowledgment reuses this exact payload or resumes
    // from the committed complement; result uniqueness handles recomputation.
    s.receipt(grant.scope,grant.owner,"checkpoint",request,payload,{});
    Statement record(s.db.handle(),"INSERT INTO checkpoints VALUES(?,?,?,'checkpoint',?,?)");
    scope_bind(record,grant.scope);record.bind(3,grant.owner);record.bind(4,request);record.bind(5,payload);record.step();
    if(s.remote(grant.scope)&&!s.importing_remote){
        // Coverage, results, audit receipt and upload pages share this commit.
        // Coverage goes on the last page so paged upload cannot finish a block
        // before all results from that checkpoint have reached the coordinator.
        int64_t added=0;
        const size_t count=std::max(size_t(1),(matches.size()+511)/512);
        for(size_t i=0;i<count;++i){
            const auto begin=std::min(i*512,matches.size()),end=std::min(begin+512,matches.size());
            CheckpointData page{grant.block,uint64_t(grant.generation),uint64_t(executor),grant.epoch,
                i+1==count?coverage:std::vector<ScalarInterval>{},
                std::vector<core::XPointMatch>(matches.begin()+begin,matches.begin()+end)};
            const auto encoded=encode_checkpoint(page);added+=int64_t(encoded.size());
            Statement out(s.db.handle(),"INSERT INTO worker_outbox(project,job,block,generation,payload,checksum) VALUES(?,?,?,?,?,?)");
            scope_bind(out,grant.scope);out.bind(3,grant.block);out.bind(4,grant.generation);out.bind(5,encoded);out.bind(6,digest(encoded));out.step();
        }
        Statement capacity(s.db.handle(),"UPDATE worker_settings SET outbox_bytes=outbox_bytes+? WHERE singleton=1 AND outbox_bytes+?<=outbox_limit");
        capacity.bind(1,added);capacity.bind(2,added);capacity.step();
        if(sqlite3_changes(s.db.handle())!=1)throw std::runtime_error("worker outbox full; checkpoint rolled back; synchronize before continuing");
    }
    tx.commit();
}
std::vector<StoredMatch> Journal::results(const Scope& scope,int64_t after,uint32_t limit)const{
    if(after<0 || !limit || limit>1000)throw std::invalid_argument("result page requires after>=0 and limit 1..1000");
    auto& s=*impl_;Transaction tx(s.db,false);const auto job=s.job(scope);
    Statement binding(s.db.handle(),"SELECT configuration,targets FROM search_bindings WHERE project=? AND job=?");scope_bind(binding,scope);
    if(!binding.step())throw std::invalid_argument("job has no verified search binding");
    const auto input=decode_binding(job.manifest,binding.blob(0),binding.blob(1));
    Statement q(s.db.handle(),"SELECT id,block,scalar,target FROM results WHERE project=? AND job=? AND id>? ORDER BY id LIMIT ?");
    scope_bind(q,scope);q.bind(3,after);q.bind(4,int64_t(limit));std::vector<StoredMatch> out;
    const size_t width=target_width(input.mode);core::XPointVerifier verifier;
    while(q.step()){
        const auto target=q.integer(3);if(target<0 || uint64_t(target)>=input.count())throw std::runtime_error("corrupt result target");
        input.verify(verifier,q.wide(2),uint32_t(target));
        if(!job.grid.block(q.wide(1)).contains(q.wide(2)))throw std::runtime_error("corrupt result block");
        out.push_back({q.integer(0),q.wide(1),q.wide(2),uint32_t(target),Bytes(input.targets.begin()+width*target,input.targets.begin()+width*(target+1))});
    }
    tx.commit();return out;
}
BlockState Journal::block(const Scope& scope,const UInt256& id)const{
    auto& s=*impl_;Transaction tx(s.db,false);const auto job=s.job(scope);const auto interval=job.grid.block(id);BlockState result;
    result.assignment=s.assignment(scope,job,id,&result.started);
    if(result.assignment){result.state="in_progress";result.expired=result.assignment->expires<=s.now();result.covered=s.coverage(scope,id);}
    else{
        Statement done(s.db.handle(),"SELECT begin FROM finished WHERE project=? AND job=? AND begin<=? AND end>? ORDER BY begin DESC LIMIT 1");scope_bind(done,scope);done.bind(3,id);done.bind(4,id);
        result.state=done.step()?"finished":"unexplored";if(result.state=="finished")result.covered.push_back(interval);
    }
    auto cursor=interval.begin();for(const auto& v:result.covered){if(!interval.contains(v)||v.begin()<cursor)throw std::runtime_error("invalid stored coverage");if(cursor<v.begin())result.remaining.emplace_back(cursor,v.begin());cursor=v.end();}if(cursor<interval.end())result.remaining.emplace_back(cursor,interval.end());tx.commit();return result;
}
Statistics Journal::statistics(const Scope& scope)const{
    auto& s=*impl_;Transaction tx(s.db,false);const auto job=s.job(scope);Statistics out;out.blocks=job.grid.count();out.unexplored=FreeTree(s.db,scope,job.grid.count()).free();out.quarantined=s.db.metadata("quarantine")!=Bytes{0};
    auto count=[&](const std::string& table){Statement q(s.db.handle(),"SELECT count(*) FROM "+table+" WHERE project=? AND job=?");scope_bind(q,scope);q.step();return uint64_t(q.integer(0));};
    out.assignments=count("assignments");out.coverage_intervals=count("coverage");out.finished_runs=count("finished");out.tree_nodes=count("free_nodes");out.requests=count("requests");out.events=count("events");
    Statement completed(s.db.handle(),"SELECT begin,end FROM finished WHERE project=? AND job=? ORDER BY begin");scope_bind(completed,scope);while(completed.step())out.finished=out.finished.add(completed.wide(1).subtract(completed.wide(0)));
    if(out.unexplored.add(out.finished).add(UInt256(out.assignments))!=out.blocks)throw std::runtime_error("journal state totals disagree");
    Statement pages(s.db.handle(),"PRAGMA freelist_count");pages.step();out.free_pages=uint64_t(pages.integer(0));
    auto size=[](const std::filesystem::path& p){return std::filesystem::exists(p)?uint64_t(std::filesystem::file_size(p)):0;};out.database_bytes=size(s.db.directory()/"progress.sqlite");out.wal_bytes=size(s.db.directory()/"progress.sqlite-wal");tx.commit();return out;
}
void Journal::check()const{
    auto& s=*impl_;Transaction tx(s.db,false);s.db.check();
    Statement jobs(s.db.handle(),"SELECT project,job FROM jobs");
    while(jobs.step()){
        const Scope scope{jobs.text(0),fixed(jobs.blob(1))};const auto job=s.job(scope);
        Statement binding(s.db.handle(),"SELECT configuration,targets,next_executor FROM search_bindings WHERE project=? AND job=?");scope_bind(binding,scope);
        if(binding.step()){
            const auto input=decode_binding(job.manifest,binding.blob(0),binding.blob(1));
            std::vector<ScalarInterval> expected_coverage,actual_coverage;
            using MatchKey=std::pair<UInt256,uint32_t>;
            std::map<MatchKey,UInt256> expected_matches,actual_matches;
            Statement receipts(s.db.handle(),"SELECT c.payload,r.payload FROM checkpoints c JOIN requests r USING(project,job,owner,operation,request) WHERE c.project=? AND c.job=?");
            scope_bind(receipts,scope);uint64_t receipt_count=0;
            while(receipts.step()){
                ++receipt_count;const auto payload=receipts.blob(0);
                if(digest(payload)!=receipts.blob(1))throw std::runtime_error("checkpoint receipt checksum mismatch");
                const auto data=decode_checkpoint(payload);const auto interval=job.grid.block(data.block);
                if(data.generation>=uint64_t(job.generation) || data.executor>=uint64_t(binding.integer(2)))throw std::runtime_error("checkpoint generation outside history");
                for(const auto& v:data.coverage){if(!interval.contains(v))throw std::runtime_error("checkpoint coverage escaped its block");expected_coverage.push_back(v);}
                for(const auto& m:data.matches){if(!interval.contains(m.scalar) || m.target>=input.count())throw std::runtime_error("checkpoint match escaped its block/targets");expected_matches.emplace(MatchKey{m.scalar,m.target},data.block);}
            }
            Statement count(s.db.handle(),"SELECT count(*) FROM requests WHERE project=? AND job=? AND operation='checkpoint'");scope_bind(count,scope);count.step();
            if(uint64_t(count.integer(0))!=receipt_count)throw std::runtime_error("missing checkpoint payload");
            Statement partial(s.db.handle(),"SELECT begin,end FROM coverage WHERE project=? AND job=?");scope_bind(partial,scope);
            while(partial.step())actual_coverage.emplace_back(partial.wide(0),partial.wide(1));
            Statement done(s.db.handle(),"SELECT begin,end FROM finished WHERE project=? AND job=?");scope_bind(done,scope);
            while(done.step())actual_coverage.emplace_back(job.grid.block(done.wide(0)).begin(),job.grid.block(done.wide(1).subtract(UInt256(1))).end());
            const auto expected=merged(expected_coverage),actual=merged(actual_coverage);
            if(expected.size()!=actual.size())throw std::runtime_error("coverage disagrees with checkpoint receipts");
            for(size_t i=0;i<expected.size();++i)if(expected[i].begin()!=actual[i].begin() || expected[i].end()!=actual[i].end())throw std::runtime_error("coverage disagrees with checkpoint receipts");
            core::XPointVerifier verifier;
            Statement matches(s.db.handle(),"SELECT block,scalar,target FROM results WHERE project=? AND job=?");scope_bind(matches,scope);
            while(matches.step()){
                const auto target=matches.integer(2);if(target<0 || uint64_t(target)>=input.count())throw std::runtime_error("invalid stored target");
                input.verify(verifier,matches.wide(1),uint32_t(target));
                actual_matches.emplace(MatchKey{matches.wide(1),uint32_t(target)},matches.wide(0));
            }
            if(expected_matches!=actual_matches)throw std::runtime_error("results disagree with checkpoint receipts");
        }
        using Range=std::pair<UInt256,UInt256>;
        std::vector<Range> occupied;
        Statement active(s.db.handle(),"SELECT block FROM assignments WHERE project=? AND job=?");scope_bind(active,scope);
        while(active.step()){
            const auto id=active.wide(0);const auto interval=job.grid.block(id);
            occupied.emplace_back(id,id.add(UInt256(1)));
            auto covered=s.coverage(scope,id);std::optional<UInt256> previous;
            for(const auto& v:covered){
                if(!interval.contains(v) || (previous && *previous>=v.begin()))throw std::runtime_error("noncanonical partial coverage");
                previous=v.end();
            }
            if(covered.size()==1 && covered[0].begin()==interval.begin() && covered[0].end()==interval.end())throw std::runtime_error("complete block still active");
        }
        Statement finished(s.db.handle(),"SELECT begin,end FROM finished WHERE project=? AND job=? ORDER BY begin");scope_bind(finished,scope);
        std::optional<UInt256> previous;
        while(finished.step()){
            const auto lo=finished.wide(0),hi=finished.wide(1);
            if(lo>=hi || hi>job.grid.count() || (previous && *previous>=lo))throw std::runtime_error("noncanonical finished runs");
            occupied.emplace_back(lo,hi);previous=hi;
        }
        std::sort(occupied.begin(),occupied.end());std::vector<UInt256> prefix{UInt256()};
        for(size_t i=0;i<occupied.size();++i){
            if(i && occupied[i-1].second>occupied[i].first)throw std::runtime_error("overlapping authoritative block states");
            prefix.push_back(prefix.back().add(occupied[i].second.subtract(occupied[i].first)));
        }
        // Audit against authoritative reservations/completed runs, independently
        // of the index's own counts. Prefix sums keep this sparse in touched rows.
        auto before=[&](const UInt256& point){
            const auto it=std::lower_bound(occupied.begin(),occupied.end(),point,[](const Range& r,const UInt256& p){return r.first<p;});
            const size_t n=size_t(it-occupied.begin());
            if(!n)return UInt256();
            return prefix[n-1].add(std::min(point,occupied[n-1].second).subtract(occupied[n-1].first));
        };
        FreeTree tree(s.db,scope,job.grid.count());
        if(tree.free()!=job.grid.count().subtract(prefix.back()))throw std::runtime_error("free-tree root disagrees with ownership");
        for(const auto& range:occupied)if(!tree.count(range.first,range.second).is_zero())throw std::runtime_error("owned range marked unexplored");
        Statement nodes(s.db.handle(),"SELECT lo,depth,free FROM free_nodes WHERE project=? AND job=?");scope_bind(nodes,scope);
        while(nodes.step()){
            const auto key=nodes.wide(0);UInt256 lo,hi=job.grid.count();const auto depth=nodes.integer(1);
            for(int64_t d=0;d<depth;++d){
                if(hi.subtract(lo)==UInt256(1))throw std::runtime_error("free-tree depth below leaf");
                const auto mid=lo.add(hi.subtract(lo).divmod(UInt256(2)).first);if(key<mid)hi=mid;else lo=mid;
            }
            if(key!=lo || nodes.wide(2)!=hi.subtract(lo).subtract(before(hi).subtract(before(lo))))throw std::runtime_error("free-tree subtree disagrees with ownership");
        }
    }
    Statement worker(s.db.handle(),"SELECT outbox_bytes FROM worker_settings WHERE singleton=1");
    if(worker.step()){
        Statement total(s.db.handle(),"SELECT COALESCE(sum(length(payload)),0) FROM worker_outbox");total.step();
        if(worker.integer(0)!=total.integer(0))throw std::runtime_error("worker outbox byte accounting mismatch");
        Statement pages(s.db.handle(),"SELECT block,generation,payload,checksum FROM worker_outbox");
        while(pages.step()){
            const auto payload=pages.blob(2);
            if(digest(payload)!=pages.blob(3))throw std::runtime_error("worker outbox checksum mismatch");
            const auto page=decode_checkpoint(payload);
            if(page.block!=pages.wide(0)||page.generation!=uint64_t(pages.integer(1)))throw std::runtime_error("worker outbox identity mismatch");
        }
    }
    tx.commit();
}
std::string Journal::state_directory()const{return impl_->db.directory().string();}
void Journal::compact(){auto& db=impl_->db;db.writable();{Statement q(db.handle(),"PRAGMA wal_checkpoint(TRUNCATE)");if(!q.step()||q.integer(0)!=0)throw std::runtime_error("checkpoint busy; retry compaction later");}db.exec("VACUUM");{Statement q(db.handle(),"PRAGMA wal_checkpoint(TRUNCATE)");if(!q.step()||q.integer(0)!=0)throw std::runtime_error("checkpoint busy after compaction");}}
void Journal::backup(const std::string& destination)const{impl_->db.backup(destination);}
void Journal::restore(const std::string& source,const std::string& destination){Database::restore(source,destination);}
} // namespace keyhunt::storage
