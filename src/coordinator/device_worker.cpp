#include "device_worker.h"
#include "self_test.h"
#include "protocol.h"
#include "checkpoint_control.h"
#include "keyhunt/coordinator/worker.h"
#include "keyhunt/backend/device.h"
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

namespace keyhunt::coordination {
namespace {
using namespace storage;
using namespace backend;
using Clock=std::chrono::steady_clock;
using Options=std::map<std::string,std::string>;
std::string option(const Options& options,const std::string& key,const std::string& fallback=""){
    const auto found=options.find(key);return found==options.end()?fallback:found->second;
}
uint64_t number(const Options& options,const std::string& key,uint64_t fallback,uint64_t maximum){
    const auto text=option(options,key,std::to_string(fallback));uint64_t value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||!value||value>maximum)
        throw std::invalid_argument("invalid --"+key);
    return value;
}
void emit(Json event){
    std::cout<<event.dump()<<'\n'<<std::flush;
    if(!std::cout)throw std::runtime_error("device event output failed; durable state retained");
}
const char* blocked_reason(ExecutionBlocked::Reason reason){
    switch(reason){
    case ExecutionBlocked::Reason::Fence:return "fenced";
    case ExecutionBlocked::Reason::Expired:return "expired";
    case ExecutionBlocked::Reason::Revalidation:return "needs-revalidation";
    case ExecutionBlocked::Reason::Paused:return "server-paused";
    }
    return "blocked";
}
#ifdef KEYHUNT_HAS_GPU
// Immutable inputs and GPU allocations survive block handoff. The checkpoint
// owner still validates the binding and allocates a fresh executor generation
// for every grant. Only one synchronous submission may be outstanding here.
struct Prepared {
    core::XPointVerifier verifier;
    std::unique_ptr<core::XPointTargets> x_targets;
    std::unique_ptr<core::BsgsPublicKeyTargets> b_targets;
    std::unique_ptr<bsgs::Table> table;
    std::unique_ptr<GpuXPointExecutor> x_executor;
    std::unique_ptr<GpuBsgsExecutor> b_executor;
    std::optional<Scope> scope;
    bool in_flight=false;
    uint64_t setups=0;
    void prepare(Worker& worker,const Grant& grant,const Options& options,uint64_t host_memory){
        if(scope){
            if(scope->project!=grant.scope.project||scope->job!=grant.scope.job)
                throw std::runtime_error("device queue changed immutable job");
            return;
        }
        const auto inputs=worker.execution(grant);
        const auto raw=wire::unhex(inputs["targets"].get<std::string>());
        if(raw.size()>host_memory/4)throw std::runtime_error("target preparation exceeds host memory budget");
        if(inputs["mode"]=="xpoint"){
            std::vector<core::XPointBytes> values(raw.size()/32);
            for(size_t i=0;i<values.size();++i)std::copy_n(raw.begin()+i*32,32,values[i].begin());
            x_targets=std::make_unique<core::XPointTargets>(std::move(values));
        }else{
            std::vector<core::UncompressedPublicKey> values(raw.size()/65);
            for(size_t i=0;i<values.size();++i)std::copy_n(raw.begin()+i*65,65,values[i].begin());
            b_targets=std::make_unique<core::BsgsPublicKeyTargets>(std::move(values));
            const auto file=option(options,"table");if(file.empty())throw std::invalid_argument("BSGS device requires --table");
            table=std::make_unique<bsgs::Table>(bsgs::Table::load(file,{16,host_memory-4*raw.size()}));
        }
        scope=grant.scope;
    }
    void drain_failed_submission() noexcept{
        // A returned/taken batch leaves no GPU work referencing its grant. Keep
        // those allocations. On a submission exception, destroy and drain them
        // before CheckpointRun releases the block's OS lock.
        if(in_flight){x_executor.reset();b_executor.reset();in_flight=false;}
    }
};
#endif
}
int run_device(const Options& args){
    require_backend(option(args,"backend"));
#ifndef KEYHUNT_HAS_GPU
    discover_gpu();return 2;
#else
    const auto ordinal_text=option(args,"device");int ordinal=-1;
    const auto parsed=std::from_chars(ordinal_text.data(),ordinal_text.data()+ordinal_text.size(),ordinal);
    if(parsed.ec!=std::errc{}||parsed.ptr!=ordinal_text.data()+ordinal_text.size()||ordinal<0)
        throw std::invalid_argument("invalid device ordinal");
    const auto queue=option(args,"queue",ordinal_text);
    const auto once=option(args,"once","no"),rebind=option(args,"rebind","no");
    if((once!="yes"&&once!="no")||(rebind!="yes"&&rebind!="no"))throw std::invalid_argument("once/rebind require yes or no");
    CheckpointOptions limits;limits.concurrent_blocks=true;
    limits.xpoint_steps=number(args,"batch-size",1048576,1048576);
    limits.target_batch=uint32_t(number(args,"target-batch",64,64));
    limits.giant_steps=number(args,"giant-batch",16384,1048576/limits.target_batch);
    const auto host_memory=number(args,"host-memory",1073741824,UINT64_MAX);
    const auto kernel=option(args,"kernel","stepped"),group=option(args,"group-size","auto");
    if(kernel!="direct"&&kernel!="stepped")throw std::invalid_argument("invalid kernel");
    if(group!="auto"&&group!="1"&&group!="8")throw std::invalid_argument("invalid group size");
    Worker worker(option(args,"state-dir"));const auto selected=select_gpu(ordinal);
    if(selected.device.uuid.empty())throw std::runtime_error("device has no stable UUID");
    worker.acquire_device(queue,selected.device.uuid,rebind=="yes");
    LocalCheckpointControl control(worker.journal().state_directory(),ordinal,selected.visible_devices,queue);
    auto callbacks=control.callbacks();
    callbacks.notify(CheckpointActivity::Running);
    // The self-test is fresh in this process and runs while this device's slot
    // is exclusively owned. No persisted pass can bypass a changed runtime.
    emit({{"type","self-test-start"},{"queue",queue}});
    const auto checked=device_self_test(ordinal);
    emit({{"type","ready"},{"queue",queue},{"self_test",checked},{"uuid",selected.device.uuid}});
    Prepared prepared;uint64_t progress=0,completed=0;
    std::optional<CheckpointActivity> idle_state;
    const auto idle=[&](CheckpointActivity activity){
        if(idle_state!=activity){control.bind(nullptr);callbacks.notify(activity);idle_state=activity;}
    };
    const auto begin=Clock::now();
    while(true){
        const auto request=callbacks.poll();
        if(request==CheckpointRequest::Stop)break;
        if(request==CheckpointRequest::Pause){idle(CheckpointActivity::Paused);callbacks.wait();continue;}
        const auto grant=worker.claim_device();
        if(!grant){
            idle(CheckpointActivity::Idle);
            if(once=="yes")break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;
        }
        idle_state.reset();control.bind(&*grant);callbacks.notify(CheckpointActivity::Running);
        emit({{"type","grant-start"},{"queue",queue},{"grant",wire::grant(*grant)}});
        try{
            prepared.prepare(worker,*grant,args,host_memory);
            const auto notify=[&](const auto&,size_t matches,double milliseconds){
                emit({{"type","checkpoint"},{"sequence",++progress},{"matches",matches},{"transaction_ms",milliseconds}});
            };
            const auto batch_done=[&](const auto& result){
                prepared.in_flight=false;
                emit({{"type","progress"},{"sequence",++progress},{"device_steps",result.device_steps},{"kernel_ms",result.kernel_ms}});
            };
            const auto cleanup=[&]{prepared.drain_failed_submission();};
            CheckpointSummary result;
            if(prepared.x_targets){
                result=CheckpointRun::xpoint(worker.journal(),*grant,*prepared.x_targets,prepared.verifier,[&](const auto& batch){
                    if(!prepared.x_executor){
                        XPointOptions gpu;gpu.max_steps=limits.xpoint_steps;
                        gpu.kernel=kernel=="direct"?XPointKernel::Direct:XPointKernel::Stepped;
                        prepared.x_executor=std::make_unique<GpuXPointExecutor>(ordinal,*prepared.x_targets,prepared.verifier,gpu);++prepared.setups;
                    }
                    prepared.in_flight=true;const auto ticket=prepared.x_executor->submit(batch);
                    prepared.x_executor->drain();auto done=prepared.x_executor->take(ticket);batch_done(done);return done;
                },limits,notify,cleanup,callbacks);
            }else{
                result=CheckpointRun::bsgs(worker.journal(),*grant,*prepared.b_targets,*prepared.table,prepared.verifier,[&](const auto& batch){
                    if(!prepared.b_executor){
                        BsgsSearchOptions gpu;gpu.max_steps=limits.giant_steps*limits.target_batch;
                        gpu.host_memory_bytes=host_memory;gpu.group_size=group=="auto"?0:group=="1"?1:8;
                        prepared.b_executor=std::make_unique<GpuBsgsExecutor>(ordinal,*prepared.table,*prepared.b_targets,prepared.verifier,gpu);++prepared.setups;
                    }
                    prepared.in_flight=true;const auto ticket=prepared.b_executor->submit(batch);
                    prepared.b_executor->drain();auto done=prepared.b_executor->take(ticket);batch_done(done);return done;
                },limits,notify,cleanup,callbacks);
            }
            if(result.complete)++completed;
            emit({{"type","grant-finish"},{"complete",result.complete},{"grant",wire::grant(*grant)},
                {"computed_scalars",result.computed_scalars.hex()},{"device_steps",result.device_steps.hex()},
                {"kernel_ms",result.kernel_ms},{"executor_setups",prepared.setups},
                {"table_upload_ms",prepared.b_executor?prepared.b_executor->table_upload_ms():0}});
            if(!result.complete)break;
        }catch(const ExecutionBlocked& blocked){
            emit({{"type","blocked"},{"reason",blocked_reason(blocked.reason)},{"message",blocked.what()}});
            idle(CheckpointActivity::Idle);
            // A current control or lease gate must be rechecked by claim_device;
            // keep the process and prepared table while synchronization resolves it.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    callbacks.notify(CheckpointActivity::Stopped);
    emit({{"type","exit"},{"reason","drained"},{"completed",completed},{"executor_setups",prepared.setups},
        {"wall_ms",std::chrono::duration<double,std::milli>(Clock::now()-begin).count()}});
    return 0;
#endif
}
}
