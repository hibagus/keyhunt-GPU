#include "keyhunt/scheduler/xpoint_batch_size.h"
#include "keyhunt/storage/checkpoint.h"
#include "checkpoint_data.h"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <thread>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace keyhunt::storage {
namespace {
using Clock=std::chrono::steady_clock;
bool same(const ScalarInterval& a,const ScalarInterval& b){return a.begin()==b.begin() && a.end()==b.end();}
class RunLock {
public:
    explicit RunLock(const std::string& directory){
        // One stable inode avoids unlink/recreate races and per-block lock-file
        // growth. C20 can replace this coarse standalone guard with its supervisor.
        fd_=::open((directory+"/executor.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd_<0)throw std::runtime_error("cannot open local executor lock");
        struct stat st{};
        if(fstat(fd_,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077) || st.st_nlink!=1 ||
           flock(fd_,LOCK_EX|LOCK_NB)){
            close(fd_);fd_=-1;throw std::runtime_error("local executor lock is unsafe or already held");
        }
    }
    ~RunLock(){if(fd_>=0)close(fd_);}
    RunLock(const RunLock&)=delete;
private:int fd_=-1;
};
struct Cleanup {
    CheckpointCleanup stop;
    ~Cleanup(){if(stop)stop();} // a throwing stop is a fatal ownership-contract violation
};
void options(const CheckpointOptions& o,bool bsgs){
    if(o.checkpoint_seconds>60 || !o.candidate_capacity ||
       o.candidate_capacity>(bsgs?65536U:1048576U))throw std::invalid_argument("invalid checkpoint interval/candidate capacity");
    if(bsgs){
        if(!o.target_batch || o.target_batch>64 || !o.giant_steps || o.giant_steps>1048576/o.target_batch)
            throw std::invalid_argument("invalid checkpoint BSGS batch limits");
    }else if(!o.xpoint_steps || o.xpoint_steps>1048576)throw std::invalid_argument("invalid checkpoint xpoint batch size");
}
void counts(bool overflow,uint64_t verified,uint64_t device,uint64_t candidates,size_t matches,
    uint64_t expected,uint64_t maximum_candidates,uint32_t capacity){
    if(device!=expected || candidates>maximum_candidates || overflow!=(candidates>capacity) ||
       (overflow?(verified!=0 || matches!=0):(verified!=expected || candidates!=matches)))
        throw std::runtime_error("inconsistent checkpoint executor completion");
}
}
struct CheckpointRun::Impl {
    Journal& journal;
    const Grant& grant;
    const detail::Binding input;
    const core::XPointVerifier& verifier;
    CheckpointOptions options;
    CheckpointObserver observer;
    CheckpointControl control;
    RunLock lock;
    CheckpointSummary summary;
    std::vector<ScalarInterval> remaining,pending;
    int64_t executor=0;
    std::string session=detail::uuid();
    uint64_t sequence=0;
    Clock::time_point last=Clock::now();

    Impl(Journal& j,const Grant& g,detail::Binding b,const core::XPointVerifier& v,
         CheckpointOptions o,CheckpointObserver notify,CheckpointControl controls)
        :journal(j),grant(g),input(std::move(b)),verifier(v),options(o),
         observer(std::move(notify)),control(std::move(controls)),lock(j.state_directory()){
        // Full semantic audit precedes any GPU work. This checks receipt hashes,
        // stored result relations and exact coverage, not only SQLite page health.
        journal.check();
        journal.bind_search(g.scope,input);
        const auto manifest=journal.manifest(g.scope);
        if(!same(scheduler::BlockGrid(manifest.root,manifest.block_width).block(g.block),g.interval))
            throw std::invalid_argument("grant bounds differ from the immutable job grid");
        const auto state=journal.block(g.scope,g.block);
        for(const auto& covered:state.covered)summary.resumed_scalars=summary.resumed_scalars.add(covered.size());
        remaining=state.remaining;
        // A retry after the final commit is an inspection, never a new executor.
        if(state.state!="finished")executor=journal.begin_search(g);
    }
    void validate(){journal.validate_search(grant,executor);}
    template<class Result> void account(const Result& result){
        // Receipts have already passed identity/count validation. These timings
        // are diagnostic only and cannot change the durable coverage decision.
        summary.device_steps=summary.device_steps.add(UInt256(result.device_steps));
        summary.verified_device_steps=summary.verified_device_steps.add(UInt256(result.verified_steps));
        summary.kernel_ms+=result.kernel_ms;summary.download_ms+=result.download_ms;
        summary.seed_ms+=result.seed_ms;summary.verification_ms+=result.verification_ms;
        summary.executor_wall_ms+=result.wall_ms;
        if(result.overflow)summary.replay_kernel_ms+=result.kernel_ms;
        summary.download_bytes=summary.download_bytes.add(UInt256(result.download_bytes));
        summary.peak_device_allocation_bytes=std::max(summary.peak_device_allocation_bytes,result.device_allocation_bytes);
        summary.peak_pinned_allocation_bytes=std::max(summary.peak_pinned_allocation_bytes,result.pinned_allocation_bytes);
    }
    void verified(const ScalarInterval& interval,const std::vector<core::XPointMatch>& matches,bool unique_target){
        const auto start=Clock::now();
        std::set<UInt256> scalars;std::set<uint32_t> targets;
        for(const auto& m:matches){
            if(!interval.contains(m.scalar) || (unique_target?!targets.insert(m.target).second:!scalars.insert(m.scalar).second))
                throw std::runtime_error("duplicate or out-of-batch checkpoint match");
            input.verify(verifier,m.scalar,m.target);
        }
        // The journal owner checks matches again after the executor. Keep that
        // cost separate from executor verification and commit_search's work.
        summary.revalidation_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
        if(detail::transaction_test_hook)detail::transaction_test_hook("after_verification");
#endif
    }
    void cover(const ScalarInterval& interval){
        // Only an exhaustive all-target interval reaches this method. Adjacent
        // no-match batches cost one pending interval, not one allocation per batch.
        if(pending.size()==1024)flush({},true);
        pending.push_back(interval);pending=detail::merged(std::move(pending));
        summary.computed_scalars=summary.computed_scalars.add(interval.size());
    }
    void flush(const std::vector<core::XPointMatch>& matches,bool force=false){
        if(pending.empty() && matches.empty())return;
        const auto now=Clock::now();
        if(!force && matches.empty() && now-last<std::chrono::seconds(options.checkpoint_seconds))return;
        const auto start=Clock::now();
        journal.commit_search(grant,executor,pending,matches,session+"."+std::to_string(++sequence));
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        ++summary.checkpoints;summary.checkpoint_ms+=ms;last=Clock::now();
        // The observer cannot make a committed checkpoint disappear if it fails.
        if(observer)observer(pending,matches.size(),ms);
        pending.clear();
    }
    void activity(CheckpointActivity value){if(control.notify)control.notify(value);}
    bool boundary(){
        if(!control.poll)return true;
        auto request=control.poll();
        if(request==CheckpointRequest::Run)return true;
        activity(CheckpointActivity::Draining);
        // The synchronous runner has drained and returned all candidates. Flush
        // exhaustive intervals even when the normal checkpoint timer is not due.
        // A partial BSGS tile has durable matches, but no scalar coverage yet.
        flush({},true);
        summary.complete=journal.block(grant.scope,grant.block).state=="finished";
        if(summary.complete){activity(CheckpointActivity::Completed);return false;}
        if(request==CheckpointRequest::Pause){
            activity(CheckpointActivity::Paused);
            while((request=control.poll())==CheckpointRequest::Pause){
                if(control.wait)control.wait();
                else std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if(request==CheckpointRequest::Run){
                // Pausing never renews a deadline or releases ownership. Audit
                // saved state, then reject expiry, recovery or executor fencing
                // before exposing the retained cursor to another submission.
                journal.check();validate();
                activity(CheckpointActivity::Running);
                return true;
            }
        }
        activity(CheckpointActivity::Stopped);
        return false;
    }
    CheckpointSummary finish(){
        flush({},true);
        if(journal.block(grant.scope,grant.block).state!="finished")throw std::logic_error("checkpointed block is incomplete");
        summary.complete=true;
        return summary;
    }
};
Scope CheckpointRun::create_xpoint(Journal& journal,const std::string& project,ScalarInterval root,UInt256 width,const core::XPointTargets& targets){
    const auto input=detail::binding(targets);
    const auto scope=journal.create_job(project,{Mode::XPoint,root,width,input.target_digest,input.algorithm_digest});
    journal.bind_search(scope,input);return scope;
}
Scope CheckpointRun::create_bsgs(Journal& journal,const std::string& project,ScalarInterval root,UInt256 width,const core::BsgsPublicKeyTargets& targets,const bsgs::Table& table){
    const auto input=detail::binding(targets,table);
    const auto scope=journal.create_job(project,{Mode::Bsgs,root,width,input.target_digest,input.algorithm_digest});
    journal.bind_search(scope,input);return scope;
}
CheckpointSummary CheckpointRun::xpoint(Journal& journal,const Grant& grant,const core::XPointTargets& targets,
    const core::XPointVerifier& verifier,const XPointRunner& run,CheckpointOptions o,CheckpointObserver observer,CheckpointCleanup cleanup,CheckpointControl control){
    options(o,false);Impl state(journal,grant,detail::binding(targets),verifier,o,std::move(observer),std::move(control));
    Cleanup stopped_before_unlock{std::move(cleanup)};
    const auto manifest=journal.manifest(grant.scope);
    scheduler::BlockGrid grid(manifest.root,manifest.block_width);
    scheduler::ExecutionIdentity identity;
    identity.job_digest=grant.scope.job;identity.target_digest=manifest.targets;identity.algorithm_digest=manifest.algorithm;
    if(grant.epoch.size()!=16)throw std::invalid_argument("invalid journal epoch");
    std::copy(grant.epoch.begin(),grant.epoch.end(),identity.assignment_id.begin());
    identity.assignment_generation=uint64_t(grant.generation);identity.executor_generation=uint64_t(state.executor);
    scheduler::XPointBatchSize sizing(o.xpoint_steps,o.candidate_capacity);
    for(const auto& gap:state.remaining){
        auto cursor=gap.begin();
        while(cursor<gap.end()){
            if(!state.boundary())return state.summary;
            state.validate();const auto steps=std::min(UInt256(sizing.limit()),gap.end().subtract(cursor)).to_uint64();
            const auto work=scheduler::WorkUnit::plan(grid,grant.block,cursor,steps,identity);
            const auto batch=*scheduler::KernelBatch::plan(*work,cursor,steps);
            const auto result=run(batch);++state.summary.batches;
            if(!same(result.batch.interval(),batch.interval()) || result.batch.work().identity()!=identity ||
               result.batch.work().block_id()!=grant.block || !same(result.batch.work().block_interval(),grant.interval))
                throw std::runtime_error("xpoint completion does not match submitted checkpoint work");
            counts(result.overflow,result.verified_steps,result.device_steps,result.candidate_count,result.matches.size(),
                batch.step_count(),batch.step_count(),o.candidate_capacity);
            state.account(result);
            if(result.overflow){
                ++state.summary.overflows;
                sizing.overflow(steps);continue;
            }
            sizing.accepted(result.candidate_count);
            state.verified(batch.interval(),result.matches,false);
            state.summary.match_observations+=result.matches.size();state.cover(batch.interval());
            state.flush(result.matches);cursor=batch.interval().end();
        }
    }
    if(!state.boundary())return state.summary;
    return state.finish();
}
CheckpointSummary CheckpointRun::bsgs(Journal& journal,const Grant& grant,const core::BsgsPublicKeyTargets& targets,const bsgs::Table& table,
    const core::XPointVerifier& verifier,const BsgsRunner& run,CheckpointOptions o,CheckpointObserver observer,CheckpointCleanup cleanup,CheckpointControl control){
    options(o,true);Impl state(journal,grant,detail::binding(targets,table),verifier,o,std::move(observer),std::move(control));
    Cleanup stopped_before_unlock{std::move(cleanup)};
    for(const auto& gap:state.remaining){
        auto cursor=gap.begin();
        while(cursor<gap.end()){
            const auto tile=core::bsgs_tile(ScalarInterval(cursor,gap.end()),table.memory().m,o.giant_steps);
            uint32_t first=0,limit=o.target_batch;
            while(first<targets.values().size()){
                if(!state.boundary())return state.summary;
                state.validate();const auto count=uint32_t(std::min<size_t>(limit,targets.values().size()-first));
                const core::BsgsBatch batch(tile,table.memory().m,first,count,targets.digest(),table.checksum());
                const auto result=run(batch);++state.summary.batches;const auto& returned=result.batch;
                if(!same(returned.interval(),tile) || returned.m()!=batch.m() || returned.first_target()!=first ||
                   returned.target_count()!=count || returned.target_digest()!=batch.target_digest() ||
                   returned.table_checksum()!=batch.table_checksum())
                    throw std::runtime_error("BSGS completion does not match submitted checkpoint work");
                counts(result.overflow,result.verified_steps,result.device_steps,result.candidate_count,result.matches.size(),
                    batch.steps(),count,o.candidate_capacity);
                state.account(result);state.summary.bsgs_group_size=result.group_size;
                if(result.overflow){
                    ++state.summary.overflows;if(count==1)throw std::logic_error("single-target overflow");
                    limit=uint32_t(std::min<uint64_t>(o.candidate_capacity,count/2));continue;
                }
                std::vector<core::XPointMatch> matches;
                for(const auto& m:result.matches){
                    if(m.target<first || m.target>=first+count)throw std::runtime_error("match outside BSGS target group");
                    matches.push_back({m.scalar,m.target});
                }
                state.verified(tile,matches,true);state.summary.match_observations+=matches.size();
                first+=count;
                // Persist subgroup matches immediately; credit the scalar tile
                // only when every canonical target has completed without overflow.
                if(first==targets.values().size())state.cover(tile);
                state.flush(matches);
            }
            cursor=tile.end();
        }
    }
    if(!state.boundary())return state.summary;
    return state.finish();
}
} // namespace keyhunt::storage
