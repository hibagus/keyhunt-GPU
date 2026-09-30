#include "keyhunt/storage/journal.h"
#include "sqlite.h"
#include "free_tree.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace keyhunt::storage {
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
    if(m.mode!=Mode::XPoint && m.mode!=Mode::Bsgs)throw std::invalid_argument("unsupported journal search semantics");
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
        if(g.epoch!=db.metadata("epoch"))throw std::invalid_argument("stale journal epoch");
        auto current=assignment(g.scope,job,g.block,started);
        if(!current || current->owner!=g.owner || current->generation!=g.generation || current->interval.begin()!=g.interval.begin() || current->interval.end()!=g.interval.end())throw std::invalid_argument("stale or foreign assignment");
        if(current->expires<=now())throw std::invalid_argument("assignment expired; explicit recovery required");
        return *current;
    }
    std::vector<ScalarInterval> coverage(const Scope& scope,const UInt256& id)const{
        Statement s(db.handle(),"SELECT begin,end FROM coverage WHERE project=? AND job=? AND block=? ORDER BY begin");scope_bind(s,scope);s.bind(3,id);std::vector<ScalarInterval> out;while(s.step())out.emplace_back(s.wide(0),s.wide(1));return out;
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
    auto& s=*impl_;Transaction tx(s.db);s.db.writable();auto job=s.job(scope);const auto expires=s.expiry(lifetime);
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
    auto& s=*impl_;auto payload=grant_payload(grant);number(payload,uint64_t(lifetime));Transaction tx(s.db);s.db.writable();auto job=s.job(grant.scope);auto expires=s.expiry(lifetime);
    if(auto old=s.retry(grant.scope,grant.owner,"renew",request,payload)){auto result=s.response(grant.scope,grant.owner,job,*old).at(0);tx.commit();return result;}
    auto current=s.authorized(grant,job);current.expires=std::max(current.expires,expires);
    Statement q(s.db.handle(),"UPDATE assignments SET expires=? WHERE project=? AND job=? AND block=?");q.bind(1,current.expires);q.bind(2,grant.scope.project);q.bind(3,bytes(grant.scope.job));q.bind(4,grant.block);q.step();
    s.receipt(grant.scope,grant.owner,"renew",request,payload,s.response({current}));tx.commit();return current;
}
Grant Journal::recover(const Scope& scope,const UInt256& block,const std::string& owner,const std::string& request,bool stopped,int64_t lifetime){
    auto& s=*impl_;Bytes payload;append(payload,block);number(payload,stopped);number(payload,uint64_t(lifetime));Transaction tx(s.db);s.db.writable();auto job=s.job(scope);const auto expires=s.expiry(lifetime);
    if(auto old=s.retry(scope,owner,"recover",request,payload)){auto result=s.response(scope,owner,job,*old).at(0);tx.commit();return result;}
    auto current=s.assignment(scope,job,block);if(!current)throw std::invalid_argument("recovery needs an in_progress block");
    if(current->expires>s.now() && !stopped)throw std::invalid_argument("confirm the previous executor stopped before live transfer");
    if(job.generation==INT64_MAX)throw std::overflow_error("assignment generation exhausted");
    current->owner=owner;current->generation=job.generation++;current->expires=expires;
    Statement q(s.db.handle(),"UPDATE assignments SET owner=?,generation=?,expires=? WHERE project=? AND job=? AND block=?");q.bind(1,owner);q.bind(2,current->generation);q.bind(3,expires);q.bind(4,scope.project);q.bind(5,bytes(scope.job));q.bind(6,block);q.step();
    s.scheduler(scope,job);s.receipt(scope,owner,"recover",request,payload,s.response({*current}));tx.commit();return *current;
}
void Journal::return_unstarted(const Grant& grant,const std::string& request){
    auto& s=*impl_;const auto payload=grant_payload(grant);Transaction tx(s.db);s.db.writable();auto job=s.job(grant.scope);
    if(s.retry(grant.scope,grant.owner,"return",request,payload)){tx.commit();return;}
    bool started=false;s.authorized(grant,job,&started);
    if(started || !s.coverage(grant.scope,grant.block).empty())throw std::invalid_argument("only an unstarted spare may be returned");
    Statement d(s.db.handle(),"DELETE FROM assignments WHERE project=? AND job=? AND block=?");scope_bind(d,grant.scope);d.bind(3,grant.block);d.step();FreeTree(s.db,grant.scope,job.grid.count()).release(grant.block);
    s.receipt(grant.scope,grant.owner,"return",request,payload,{});tx.commit();
}
void Journal::record_coverage(const Grant& grant,const std::vector<ScalarInterval>& intervals,const std::string& request){
    if(intervals.empty() || intervals.size()>1024)throw std::invalid_argument("coverage batch must have 1..1024 intervals");
    auto& s=*impl_;auto payload=grant_payload(grant);for(const auto& v:intervals){append(payload,v.begin());append(payload,v.end());}
    Transaction tx(s.db);s.db.writable();auto job=s.job(grant.scope);if(s.retry(grant.scope,grant.owner,"coverage",request,payload)){tx.commit();return;}
    bool started=false;s.authorized(grant,job,&started);if(!started)throw std::invalid_argument("coverage needs a started assignment");
    auto all=s.coverage(grant.scope,grant.block);
    for(const auto& v:all)if(!grant.interval.contains(v))throw std::runtime_error("stored coverage escaped its block");
    for(const auto& v:intervals){if(!grant.interval.contains(v))throw std::invalid_argument("coverage outside assigned block");all.push_back(v);}
    std::sort(all.begin(),all.end(),[](const auto& a,const auto& b){return a.begin()<b.begin();});
    std::vector<ScalarInterval> merged;
    for(const auto& v:all){if(merged.empty() || merged.back().end()<v.begin())merged.push_back(v);else merged.back()=ScalarInterval(merged.back().begin(),std::max(merged.back().end(),v.end()));}
    Statement remove(s.db.handle(),"DELETE FROM coverage WHERE project=? AND job=? AND block=?");scope_bind(remove,grant.scope);remove.bind(3,grant.block);remove.step();
    if(merged.size()==1 && merged[0].begin()==grant.interval.begin() && merged[0].end()==grant.interval.end()){
        Statement done(s.db.handle(),"DELETE FROM assignments WHERE project=? AND job=? AND block=?");scope_bind(done,grant.scope);done.bind(3,grant.block);done.step();s.merge_finished(grant.scope,grant.block);
    }else for(const auto& v:merged){Statement q(s.db.handle(),"INSERT INTO coverage VALUES(?,?,?,?,?)");scope_bind(q,grant.scope);q.bind(3,grant.block);q.bind(4,v.begin());q.bind(5,v.end());q.step();}
    s.receipt(grant.scope,grant.owner,"coverage",request,payload,{});tx.commit();
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
    tx.commit();
}
void Journal::compact(){auto& db=impl_->db;db.writable();{Statement q(db.handle(),"PRAGMA wal_checkpoint(TRUNCATE)");if(!q.step()||q.integer(0)!=0)throw std::runtime_error("checkpoint busy; retry compaction later");}db.exec("VACUUM");{Statement q(db.handle(),"PRAGMA wal_checkpoint(TRUNCATE)");if(!q.step()||q.integer(0)!=0)throw std::runtime_error("checkpoint busy after compaction");}}
void Journal::backup(const std::string& destination)const{impl_->db.backup(destination);}
void Journal::restore(const std::string& source,const std::string& destination){Database::restore(source,destination);}
} // namespace keyhunt::storage
