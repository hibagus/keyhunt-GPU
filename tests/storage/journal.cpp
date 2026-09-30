#include "keyhunt/storage/journal.h"
#include "free_tree.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <set>
#include <unistd.h>
using namespace keyhunt;
using namespace storage;
using namespace storage::detail;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
    try {
        // Honor TMPDIR so journal fixtures can live outside a managed checkout.
        std::string temp=(std::filesystem::temp_directory_path()/"keyhunt-c12-journal-XXXXXX").string();require(mkdtemp(temp.data()),"mkdtemp");
        struct Cleanup {std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}} cleanup{temp};
        int64_t now=1000;
        Journal journal(temp,[&]{return now;});
        const auto project=journal.create_project("test"),other_project=journal.create_project("separate");
        Digest seed{};seed[31]=12;
        Manifest manifest{Mode::XPoint,ScalarInterval(UInt256(1),UInt256(102)),UInt256(10),{}, {}};
        const auto scope=journal.create_job(project,manifest,seed),other=journal.create_job(other_project,manifest,seed);
        require(scope.job==other.job,"logical identity must not include project");
        auto changed=manifest;changed.algorithm[0]=1;
        require(journal.create_job(project,changed).job!=scope.job,"algorithm identity omitted");
        require(journal.statistics(scope).tree_nodes==0,"untouched job materialized blocks");
        Selection choice;choice.count=3;
        auto grants=journal.claim(scope,"owner","first",choice,10);
        require(grants.size()==3&&grants[2].block==UInt256(2),"sequential allocation");
        auto retry=journal.claim(scope,"owner","first",choice,10);
        require(retry[0].generation==grants[0].generation&&retry[0].expires==1010,"retry changed grant");
        choice.count=2;rejects([&]{journal.claim(scope,"owner","first",choice,10);});
        auto cross=grants[0];cross.scope=other;rejects([&]{journal.start(cross,"cross");});
        auto bad_epoch=grants[0];bad_epoch.epoch[0]^=1;rejects([&]{journal.start(bad_epoch,"epoch");});
        journal.start(grants[0],"start");
        rejects([&]{journal.return_unstarted(grants[0],"started-return");});
        journal.record_coverage(grants[0],{{UInt256(3),UInt256(5)},{UInt256(1),UInt256(3)},{UInt256(7),UInt256(9)}},"partial");
        auto state=journal.block(scope,UInt256(0));
        require(state.state=="in_progress"&&state.covered.size()==2&&state.remaining.size()==2,"partial merge/complement");
        require(state.covered[0].begin()==UInt256(1)&&state.covered[0].end()==UInt256(5),"adjacent interval merge");
        rejects([&]{journal.record_coverage(grants[0],{{UInt256(1),UInt256(12)}},"outside");});
        require(journal.block(scope,UInt256(0)).covered.size()==2,"failed transaction altered coverage");
        now=1010;
        require(journal.block(scope,UInt256(0)).expired,"expiry boundary");
        rejects([&]{journal.record_coverage(grants[0],{{UInt256(1),UInt256(11)}},"expired");});
        auto fresh=journal.claim(scope,"other","after-expiry",{},10);
        require(fresh[0].block==UInt256(3),"expired blocks were reclaimed automatically");
        auto recovered=journal.recover(scope,UInt256(0),"new-owner","recover",false,10);
        require(recovered.generation>grants[0].generation&&journal.block(scope,UInt256(0)).covered.size()==2,"recovery lost coverage/fence");
        rejects([&]{journal.start(grants[0],"stale");});
        rejects([&]{journal.recover(scope,UInt256(0),"third","live-transfer",false,10);});
        journal.record_coverage(recovered,{{UInt256(5),UInt256(7)},{UInt256(9),UInt256(11)}},"finish");
        journal.record_coverage(recovered,{{UInt256(5),UInt256(7)},{UInt256(9),UInt256(11)}},"finish");
        require(journal.block(scope,UInt256(0)).state=="finished","complete interval not finished");
        auto g1=journal.recover(scope,UInt256(1),"new-owner","recover1",false,10);
        journal.start(g1,"start1");journal.record_coverage(g1,{g1.interval},"finish1");
        require(journal.statistics(scope).finished_runs==1&&journal.statistics(scope).finished==UInt256(2),"completed runs not coalesced");
        auto spare=journal.recover(scope,UInt256(2),"new-owner","recover2",false,10);
        journal.return_unstarted(spare,"return2");journal.return_unstarted(spare,"return2");
        auto replacement=journal.claim(scope,"new-owner","reclaim2",{},10).at(0);
        require(replacement.block==UInt256(2)&&replacement.generation>spare.generation,"returned block reused generation");
        rejects([&]{journal.start(spare,"old-returned");});
        auto renewed=journal.renew(replacement,"renew",20);now=1011;
        require(journal.renew(replacement,"renew",20).expires==renewed.expires,"renew retry extended deadline");
        require(journal.statistics(other).unexplored==UInt256(11),"project state leaked");
        journal.check();
        // Exhaust every occupancy subset and every rank/window in tiny trees.
        // The independent reference is a vector of free IDs, not tree traversal.
        Database db(temp);
        unsigned patterns=0,rank_checks=0;
        for(uint64_t n=1;n<=7;++n){
            const auto tiny=journal.create_job(project,{Mode::Bsgs,ScalarInterval(UInt256(1),UInt256(n+1)),UInt256(1),{},{}},seed);
            for(uint64_t mask=0;mask<(1ULL<<n);++mask){
                Transaction tx(db);FreeTree tree(db,tiny,UInt256(n));std::vector<uint64_t> expected;
                for(uint64_t id=0;id<n;++id)if(mask&(1ULL<<id))tree.occupy(UInt256(id));else expected.push_back(id);
                require(tree.free()==UInt256(expected.size()),"free count differs");
                for(size_t rank=0;rank<expected.size();++rank){require(tree.select(UInt256(rank))==UInt256(expected[rank]),"rank differs");++rank_checks;}
                rejects([&]{tree.select(UInt256(expected.size()));});
                for(uint64_t begin=0;begin<=n;++begin)for(uint64_t end=begin;end<=n;++end){
                    auto count=std::count_if(expected.begin(),expected.end(),[&](auto id){return begin<=id&&id<end;});
                    require(tree.count(UInt256(begin),UInt256(end))==UInt256(uint64_t(count)),"window count differs");
                }
                for(uint64_t id=0;id<n;++id)if(mask&(1ULL<<id))tree.release(UInt256(id));
                require(tree.free()==UInt256(n),"collapsed subtree release differs");++patterns;
                // Roll back each synthetic tree; it is not an assignment fixture.
            }
        }
        const auto huge=journal.create_job(project,{Mode::XPoint,ScalarInterval(UInt256(1),core::scalar_order()),UInt256(1),{},{}},seed);
        Selection manual;manual.policy=Policy::Manual;manual.block=UInt256::power_of_two(240).add(UInt256(123));
        auto high=journal.claim(huge,"wide","high",manual).at(0);require(high.block==*manual.block,"wide block truncated");
        require(journal.statistics(huge).tree_nodes<=257,"wide claim materialized theoretical grid");
        Selection window;window.policy=Policy::RandomWindow;window.window=UInt256(17);window.count=32;
        auto selected=journal.claim(huge,"wide","window",window);require(!selected.empty()&&selected.size()<=17,"window cap");
        const auto bucket=selected[0].block.divmod(UInt256(17)).first;
        for(const auto& g:selected)require(g.block.divmod(UInt256(17)).first==bucket,"random window escaped");
        // Near exhaustion, random selection must pick the last rank immediately.
        const auto near=journal.create_job(project,{Mode::Bsgs,ScalarInterval(UInt256(200),UInt256(209)),UInt256(1),{},{}},seed);
        choice={};choice.count=8;journal.claim(near,"rank","eight",choice);
        choice.policy=Policy::Random;choice.count=9;auto last=journal.claim(near,"rank","last",choice);
        require(last.size()==1&&last[0].block==UInt256(8),"random exhaustion");require(journal.claim(near,"rank","empty",choice).empty(),"empty selection");
        const auto before=journal.statistics(scope);journal.compact();const auto after=journal.statistics(scope);
        require(before.requests==after.requests&&before.finished==after.finished,"compaction lost receipts/state");
        journal.check();
        journal.backup(std::string(temp)+"/corrupt");
        {
            Database corrupt(std::string(temp)+"/corrupt");
            Statement q(corrupt.handle(),"UPDATE free_nodes SET free=? WHERE project=? AND job=? AND depth=0");
            q.bind(1,UInt256());q.bind(2,scope.project);q.bind(3,Bytes(scope.job.begin(),scope.job.end()));q.step();
        }
        Journal corrupt(std::string(temp)+"/corrupt",[&]{return now;});rejects([&]{corrupt.check();});
        std::cout<<patterns<<" exhaustive free-tree patterns, "<<rank_checks<<" ranks; scoped lifecycle, wide IDs and coverage passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
