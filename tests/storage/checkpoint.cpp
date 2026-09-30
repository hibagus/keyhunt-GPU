#include "checkpoint_fixture.h"
#include "checkpoint_data.h"
#include "schema_v1.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sys/stat.h>
using namespace fixture;
using namespace keyhunt::storage::detail;
int main(){
    try{
        Temporary temporary;const auto path=temporary.path.string();core::XPointVerifier verifier;
        auto xs=x_targets(verifier,{1,5,9});auto bs=b_targets(verifier,{1,5,9});
        auto table=bsgs::Table::build(3);Journal j(path);const auto project=j.create_project("checkpoint tests");
        CheckpointOptions options;options.xpoint_steps=4;options.giant_steps=2;options.target_batch=2;options.candidate_capacity=1;options.checkpoint_seconds=0;
        const auto scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(1),UInt256(10)),UInt256(9),xs);
        const auto grant=j.claim(scope,"worker","claim").at(0);
        rejects([&]{j.record_coverage(grant,{grant.interval},"unsafe");});
        const auto run=[&](const auto& batch){return execute(batch,xs,verifier,options.candidate_capacity);};
        unsigned acknowledgments=0;
        auto summary=CheckpointRun::xpoint(j,grant,xs,verifier,run,options,[&](const auto&,size_t,double){
            // Every acknowledgement observes a complete SQL transaction.
            ++acknowledgments;j.check();
        });
        require(summary.computed_scalars==UInt256(9)&&summary.checkpoints==acknowledgments,"xpoint union");
        auto matches=j.results(scope);require(matches.size()==3,"lost xpoint results");
        std::set<UInt256> expected{UInt256(1),UInt256(5),UInt256(9)};
        for(const auto& m:matches)require(expected.erase(m.scalar),"wrong result scalar");
        require(expected.empty(),"missing boundary matches");
        require(j.results(scope,matches[0].id,1).size()==1,"result pagination");
        auto finished=CheckpointRun::xpoint(j,grant,xs,verifier,[](const auto&)->backend::XPointResult{throw std::runtime_error("finished job executed");},options);
        require(finished.resumed_scalars==UInt256(9)&&finished.batches==0,"finished retry");
        auto different=x_targets(verifier,{2});rejects([&]{CheckpointRun::xpoint(j,grant,different,verifier,run,options);});

        // Timings and transfer costs include discarded overflow work; useful
        // steps and scalar coverage must not count it a second time.
        auto dense=x_targets(verifier,{1,2});
        const auto dense_scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(1),UInt256(9)),UInt256(8),dense);
        const auto dense_grant=j.claim(dense_scope,"worker","metrics").at(0);
        auto measured=CheckpointRun::xpoint(j,dense_grant,dense,verifier,[&](const auto& b){
            auto r=execute(b,dense,verifier,1);
            r.kernel_ms=2;r.download_ms=1;r.seed_ms=0.25;r.verification_ms=0.5;r.wall_ms=4;
            r.download_bytes=64;r.device_allocation_bytes=1024;r.pinned_allocation_bytes=128;
            return r;
        },options);
        require(measured.overflows==1 && measured.device_steps==UInt256(12) &&
                measured.verified_device_steps==UInt256(8) && measured.computed_scalars==UInt256(8),"xpoint replay accounting");
        require(measured.batches==6 && measured.kernel_ms==12 && measured.replay_kernel_ms==2 && measured.executor_wall_ms==24 &&
                measured.download_ms==6 && measured.seed_ms==1.5 && measured.verification_ms==3 &&
                measured.download_bytes==UInt256(384) && measured.peak_device_allocation_bytes==1024 &&
                measured.peak_pinned_allocation_bytes==128,"xpoint timing/transfer accounting");
        require(finished.verified_device_steps==UInt256() && finished.kernel_ms==0,"retry reported phantom GPU work");

        // A dense prefix must not reduce every subsequent work unit to one
        // scalar. Exact receipts still cover the interval once; all hits persist.
        const auto prefix=x_targets(verifier,{100,101,102,103});
        auto prefix_scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(100),UInt256(4196)),UInt256(4096),prefix);
        auto prefix_grant=j.claim(prefix_scope,"worker","prefix").at(0);
        auto recovery_options=options;recovery_options.xpoint_steps=256;
        bool recovered_batch=false;
        auto recovered_prefix=CheckpointRun::xpoint(j,prefix_grant,prefix,verifier,[&](const auto& b){
            if(b.interval().begin()>=UInt256(612) && b.step_count()==256)recovered_batch=true;
            return execute(b,prefix,verifier,1);
        },recovery_options);
        require(recovered_batch && recovered_prefix.batches<40 && recovered_prefix.overflows==1 &&
                recovered_prefix.computed_scalars==UInt256(4096) && j.results(prefix_scope).size()==4,"sparse tail did not recover");
        const auto full=x_targets(verifier,{1,2,3,4,5,6,7,8});
        auto full_scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(1),UInt256(9)),UInt256(8),full);
        auto full_grant=j.claim(full_scope,"worker","full").at(0);
        recovery_options.xpoint_steps=8;
        auto saturated=CheckpointRun::xpoint(j,full_grant,full,verifier,[&](const auto& b){return execute(b,full,verifier,1);},recovery_options);
        require(saturated.batches==9 && saturated.overflows==1 && saturated.computed_scalars==UInt256(8) &&
                j.results(full_scope).size()==8,"dense recovery repeatedly overflowed");

        // No-match batches coalesce until the timer/end boundary, avoiding a
        // FULL fsync in every short kernel loop by default.
        auto absent=x_targets(verifier,{1000});
        auto empty_scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(1),UInt256(33)),UInt256(32),absent);
        auto empty_grant=j.claim(empty_scope,"worker","no-match").at(0);options.checkpoint_seconds=60;
        auto empty=CheckpointRun::xpoint(j,empty_grant,absent,verifier,[&](const auto& b){return execute(b,absent,verifier,1);},options);
        require(empty.batches==8&&empty.checkpoints==1&&j.results(empty_scope).empty(),"no-match checkpoint batching");

        const auto bscope=CheckpointRun::create_bsgs(j,project,ScalarInterval(UInt256(1),UInt256(10)),UInt256(9),bs,table);
        const auto bg=j.claim(bscope,"worker","bsgs").at(0);options.target_batch=1;
        bool interrupted=false,cleaned=false;
        try{
            CheckpointRun::bsgs(j,bg,bs,table,verifier,[&](const auto& b){return execute(b,bs,verifier,1);},options,
                [&](const auto& covered,size_t count,double){
                    if(count && covered.empty()){interrupted=true;throw std::runtime_error("lost acknowledgement");}
                },[&]{
                    cleaned=true;
                    // Cleanup still owns the guard; a replacement cannot race
                    // with draining/destroying a failed GPU executor.
                    rejects([&]{CheckpointRun::bsgs(j,bg,bs,table,verifier,[&](const auto& b){return execute(b,bs,verifier,1);},options);});
                });
        }catch(const std::exception&){require(interrupted,"unexpected BSGS interruption");}
        require(interrupted && cleaned && !j.results(bscope).empty(),"partial-target match was not durable");
        require(j.block(bscope,UInt256()).covered.empty(),"partial target group credited scalar coverage");
        const auto wrong_table=bsgs::Table::build(4);
        rejects([&]{CheckpointRun::bsgs(j,bg,bs,wrong_table,verifier,[&](const auto& b){return execute(b,bs,verifier,1);},options);});
        options.target_batch=3;options.giant_steps=3;
        auto replay=CheckpointRun::bsgs(j,bg,bs,table,verifier,[&](const auto& b){
            auto r=execute(b,bs,verifier,1);r.kernel_ms=2;r.wall_ms=3;return r;
        },options);
        require(replay.verified_device_steps==UInt256(9) && replay.device_steps==UInt256(18) &&
                replay.computed_scalars==UInt256(9) && replay.kernel_ms==8 && replay.replay_kernel_ms==2 &&
                replay.executor_wall_ms==12,"BSGS target-giant replay accounting");
        require(replay.overflows>0&&j.results(bscope).size()==3,"BSGS replay/deduplication");
        j.check();

        // A failing verifier must not accept a result or its dependent interval.
        auto bad_scope=CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(1),UInt256(4)),UInt256(3),xs);
        auto bad=j.claim(bad_scope,"bad","claim").at(0);
        rejects([&]{CheckpointRun::xpoint(j,bad,xs,verifier,[&](const auto& b){
            auto r=execute(b,xs,verifier,1);r.matches[0].scalar=UInt256(2);return r;
        },options);});
        require(j.block(bad_scope,UInt256()).covered.empty()&&j.results(bad_scope).empty(),"invalid CPU match committed");
        // Exclusive execution, changed ownership and expiry are checked before
        // further execution and again when committing a completed batch.
        int64_t now=1000;Journal clocked(path,[&]{return now;});
        auto fence_scope=CheckpointRun::create_xpoint(clocked,project,ScalarInterval(UInt256(20),UInt256(24)),UInt256(4),absent);
        auto fence=clocked.claim(fence_scope,"worker","fence",{},2).at(0);
        bool locked=false;
        rejects([&]{CheckpointRun::xpoint(clocked,fence,absent,verifier,[&](const auto& b){
            rejects([&]{CheckpointRun::xpoint(clocked,fence,absent,verifier,run,options);});locked=true;
            now=1002;return execute(b,absent,verifier,1);
        },options);});
        require(locked&&clocked.block(fence_scope,UInt256()).covered.empty(),"expiry/lock boundary");
        auto recovered=clocked.recover(fence_scope,UInt256(),"new","recover");
        rejects([&]{CheckpointRun::xpoint(clocked,fence,absent,verifier,run,options);});
        CheckpointRun::xpoint(clocked,recovered,absent,verifier,[&](const auto& b){return execute(b,absent,verifier,1);},options);

        auto transfer_scope=CheckpointRun::create_xpoint(clocked,project,ScalarInterval(UInt256(50),UInt256(54)),UInt256(4),absent);
        auto transfer=clocked.claim(transfer_scope,"worker","transfer").at(0);
        rejects([&]{CheckpointRun::xpoint(clocked,transfer,absent,verifier,[&](const auto& b){
            // Model a control-plane fence after submission but before commit.
            clocked.recover(transfer_scope,UInt256(),"replacement","transfer",true);
            return execute(b,absent,verifier,1);
        },options);});
        require(clocked.block(transfer_scope,UInt256()).covered.empty(),"fenced completion was credited");
        rejects([&]{CheckpointRun::xpoint(j,bad,xs,verifier,[&](const auto& b){
            auto r=execute(b,xs,verifier,1);--r.verified_steps;return r;
        },options);});
        require(j.results(bad_scope).empty(),"inconsistent completion committed");

        // Binding C12 synthetic progress as verified progress is forbidden.
        const auto input=binding(absent);
        auto legacy=j.create_job(project,{Mode::XPoint,ScalarInterval(UInt256(30),UInt256(40)),UInt256(10),input.target_digest,input.algorithm_digest});
        auto old=j.claim(legacy,"old","claim").at(0);j.start(old,"start");j.record_coverage(old,{{UInt256(30),UInt256(31)}},"synthetic");
        rejects([&]{CheckpointRun::create_xpoint(j,project,ScalarInterval(UInt256(30),UInt256(40)),UInt256(10),absent);});

        // Independent receipt auditing detects missing matches and edited coverage.
        const auto corrupt_path=path+"/corrupt";j.backup(corrupt_path);
        {
            Database db(corrupt_path);db.exec("DELETE FROM results WHERE id=(SELECT min(id) FROM results)");
        }
        Journal corrupt(corrupt_path);rejects([&]{corrupt.check();});
        j.backup(path+"/coverage-corrupt");
        {
            Database db(path+"/coverage-corrupt");
            Statement q(db.handle(),"DELETE FROM finished WHERE project=? AND job=?");q.bind(1,scope.project);q.bind(2,Bytes(scope.job.begin(),scope.job.end()));q.step();
        }
        Journal coverage_corrupt(path+"/coverage-corrupt");rejects([&]{coverage_corrupt.check();});
        j.backup(path+"/receipt-corrupt");
        {Database db(path+"/receipt-corrupt");db.exec("UPDATE checkpoints SET payload=zeroblob(length(payload))");}
        Journal receipt_corrupt(path+"/receipt-corrupt");rejects([&]{receipt_corrupt.check();});
        j.backup(path+"/binding-corrupt");
        {Database db(path+"/binding-corrupt");db.exec("UPDATE search_bindings SET targets=zeroblob(length(targets))");}
        Journal binding_corrupt(path+"/binding-corrupt");rejects([&]{binding_corrupt.check();});

        // Build a real v1 file directly from the immutable published migration.
        const auto v1=temporary.path/"v1";std::filesystem::create_directory(v1);chmod(v1.c_str(),0700);
        sqlite3* raw=nullptr;require(sqlite3_open((v1/"progress.sqlite").c_str(),&raw)==SQLITE_OK,"v1 open");
        require(sqlite3_exec(raw,schema_v1,nullptr,nullptr,nullptr)==SQLITE_OK,"v1 schema");
        {Statement q(raw,"INSERT INTO migrations VALUES(1,?)");q.bind(1,digest(Bytes(schema_v1,schema_v1+std::strlen(schema_v1))));q.step();}
        require(sqlite3_exec(raw,"INSERT INTO metadata VALUES('epoch',zeroblob(16)),('quarantine',x'00'); INSERT INTO projects VALUES('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','v1 retained'); PRAGMA application_id=1263028785; PRAGMA user_version=1;",nullptr,nullptr,nullptr)==SQLITE_OK,"v1 fixture");
        sqlite3_close(raw);chmod((v1/"progress.sqlite").c_str(),0600);
        {Database migrated(v1.string());migrated.check();Statement q(migrated.handle(),"SELECT name FROM projects");require(q.step()&&q.text(0)=="v1 retained","migration lost project");}
        unsigned backups=0;for(const auto& entry:std::filesystem::directory_iterator(v1)){
            if(entry.is_directory()&&entry.path().filename().string().rfind("pre-v5-",0)==0){
                ++backups;sqlite3* snapshot=nullptr;require(sqlite3_open_v2((entry.path()/"progress.sqlite").c_str(),&snapshot,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"backup read");
                {Statement q(snapshot,"PRAGMA user_version");require(q.step()&&q.integer(0)==1,"migration backup was not v1");}
                {Statement q(snapshot,"SELECT value FROM metadata WHERE key='quarantine'");require(q.step()&&q.blob(0)==Bytes{1},"migration backup not sealed");}
                sqlite3_close(snapshot);
            }
        }
        require(backups==1,"missing pre-migration backup");
        j.check();std::cout<<"Verified checkpoints, all-target replay, fences, corruption rejection and v1 migration passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
