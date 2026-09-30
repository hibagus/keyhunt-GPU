#include "keyhunt/storage/journal.h"
#include "sqlite.h"
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
using namespace keyhunt::storage;
using namespace keyhunt::storage::detail;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){
    try {
        // Honor TMPDIR so journal fixtures can live outside a managed checkout.
        std::string temp=(std::filesystem::temp_directory_path()/"keyhunt-c12-process-XXXXXX").string();require(mkdtemp(temp.data()),"mkdtemp");
        struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{temp};
        const auto clock=[]{return int64_t(1000);};
        Scope crash_scope,parallel_scope,retry_scope;
        {
            Journal j(temp,clock);const auto project=j.create_project("process tests");
            crash_scope=j.create_job(project,{Mode::XPoint,ScalarInterval(UInt256(1),UInt256(100)),UInt256(1),{}, {}});
            parallel_scope=j.create_job(project,{Mode::XPoint,ScalarInterval(UInt256(1000),UInt256(1128)),UInt256(1),{}, {}});
            retry_scope=j.create_job(project,{Mode::Bsgs,ScalarInterval(UInt256(1000),UInt256(1128)),UInt256(1),{}, {}});
        }
        unsigned crashes=0,workers=0;
        for(const std::string boundary:{"before_commit","after_commit"}){
            const pid_t child=fork();require(child>=0,"fork");
            if(!child){
                try {
                    Journal j(temp,clock);
                    transaction_test_hook=[&](const char* at){if(at==boundary)_exit(73);};
                    (void)j.claim(crash_scope,"crash-owner",boundary);
                }catch(...){_exit(74);}
                _exit(75);
            }
            int status=0;waitpid(child,&status,0);require(WIFEXITED(status)&&WEXITSTATUS(status)==73,"crash boundary not reached");
            Journal j(temp,clock);
            require(j.statistics(crash_scope).assignments==(boundary=="before_commit"?0:2),"crash atomicity");
            const auto grant=j.claim(crash_scope,"crash-owner",boundary).at(0);
            require(grant.block==UInt256(boundary=="before_commit"?0:1),"retry did not recover original allocation");
            require(j.statistics(crash_scope).assignments==(boundary=="before_commit"?1:2),"crash retry allocated twice");
            j.check();++crashes;
        }
        // Each child opens its own SQLite connection after fork. No inherited
        // live connection is used; BEGIN IMMEDIATE makes a claim and receipt atomic.
        for(bool same_request:{false,true}){
            std::vector<pid_t> children;
            for(unsigned i=0;i<8;++i){
                const auto child=fork();require(child>=0,"fork");
                if(!child){try{
                    Journal j(temp,clock);Selection select;select.count=16;select.policy=Policy::Random;
                    auto grants=j.claim(same_request?retry_scope:parallel_scope,same_request?"same-owner":"worker-"+std::to_string(i),"request",select);
                    _exit(grants.size()==16?0:2);
                }catch(const std::exception& e){std::cerr<<e.what()<<'\n';_exit(3);}}
                children.push_back(child);
            }
            for(auto child:children){int status=0;waitpid(child,&status,0);require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"concurrent claim failed");++workers;}
            Journal j(temp,clock);auto stats=j.statistics(same_request?retry_scope:parallel_scope);
            require(stats.assignments==(same_request?16:128),"concurrent duplicate allocation");
            require(stats.requests==(same_request?1:8),"concurrent retry ledger mismatch");j.check();
        }
        std::cout<<crashes<<" real process exits around COMMIT, "<<workers<<" concurrent writer processes passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
