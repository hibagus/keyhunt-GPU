#include "state_helpers.h"
#include "checkpoint_control.h"
#include "keyhunt/storage/checkpoint.h"
#include "keyhunt/backend/device.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>

namespace keyhunt::backend {
namespace {
using namespace storage;
using namespace state_detail;
void flush(){
    std::cout<<'\n'<<std::flush;
    if(!std::cout)throw std::runtime_error("checkpoint output failed; committed state is retained");
}
uint64_t decimal(const std::string& value,uint64_t maximum=UINT64_MAX){
    uint64_t n=0;const auto result=std::from_chars(value.data(),value.data()+value.size(),n);
    if(result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || n>maximum)
        throw std::invalid_argument("invalid unsigned decimal checkpoint option");
    return n;
}
bsgs::Table load_table(const Options& options,const core::BsgsPublicKeyTargets& targets){
    const auto memory=decimal(optional(options,"host-memory","1073741824"));
    const auto target_bytes=targets.values().capacity()*sizeof(core::UncompressedPublicKey);
    if(memory<=target_bytes)throw std::invalid_argument("BSGS targets exceed host memory budget");
    return bsgs::Table::load(required(options,"table"),{16,memory-target_bytes});
}
}
int checkpoint_command(int argc,char** argv){
    static const std::map<std::string,std::set<std::string>> allowed{
        {"create",{"project","mode","range","block-width","targets","table","host-memory"}},
        {"run",{"backend","grant","targets","table","device","batch-size","kernel","giant-batch",
                "target-batch","candidate-capacity","group-size","host-memory","reserve-bytes","checkpoint-seconds"}},
        {"results",{"project","job","after","limit"}},
        {"pause",{}},{"resume",{}},{"stop",{}},{"status",{}}};
    if(argc<3)throw std::invalid_argument("usage: keyhunt checkpoint create|run|results|pause|resume|stop|status [--state-dir DIR] ...; see docs/CHECKPOINTS.md");
    const std::string action=argv[2];const auto spec=allowed.find(action);
    if(spec==allowed.end())throw std::invalid_argument("unknown checkpoint action");
    Options args;
    for(int i=3;i<argc;i+=2){
        const std::string flag=argv[i];
        if(i+1>=argc || flag.rfind("--",0)!=0)throw std::invalid_argument("checkpoint options require --name VALUE");
        const auto key=flag.substr(2);
        if(key!="state-dir" && !spec->second.count(key))throw std::invalid_argument("unsupported checkpoint option: "+flag);
        if(!args.emplace(key,argv[i+1]).second)throw std::invalid_argument("duplicate checkpoint option: "+flag);
    }
    if(action=="run") require_backend(required(args,"backend"));
    const auto wall_start=std::chrono::steady_clock::now();
    const auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count();};
    std::cout<<std::setprecision(12);
    Journal journal(optional(args,"state-dir"));
    if(action=="pause" || action=="resume" || action=="stop" || action=="status"){
        const auto response=LocalCheckpointControl::command(journal.state_directory(),action);
        std::cout<<response;flush();
        return response.find("\"accepted\":false")==std::string::npos?0:2;
    }
    if(action=="create"){
        const auto range=required(args,"range");const auto colon=range.find(':');
        if(colon==std::string::npos || range.find(':',colon+1)!=std::string::npos)throw std::invalid_argument("range must be half-open HEX:HEX");
        const ScalarInterval root(UInt256::from_hex(range.substr(0,colon)),UInt256::from_hex(range.substr(colon+1)));
        const auto width=UInt256::from_hex(required(args,"block-width"));const auto mode=required(args,"mode");
        Scope id;
        if(mode=="xpoint"){
            if(args.count("table")||args.count("host-memory"))throw std::invalid_argument("xpoint has no BSGS table/memory option");
            const auto targets=core::XPointTargets::load(required(args,"targets"));
            id=CheckpointRun::create_xpoint(journal,required(args,"project"),root,width,targets);
        }else if(mode=="bsgs"){
            const auto targets=core::BsgsPublicKeyTargets::load(required(args,"targets"));const auto table=load_table(args,targets);
            id=CheckpointRun::create_bsgs(journal,required(args,"project"),root,width,targets,table);
        }else throw std::invalid_argument("checkpoint mode must be xpoint or bsgs");
        const auto manifest=journal.manifest(id);
        std::cout<<"{\"project\":"<<quote(id.project)<<",\"job\":"<<quote(hex(id.job.data(),32))
            <<",\"target_digest\":"<<quote(hex(manifest.targets.data(),32))
            <<",\"algorithm_digest\":"<<quote(hex(manifest.algorithm.data(),32))<<"}";flush();return 0;
    }
    if(action=="results"){
        const auto after=decimal(optional(args,"after","0"),INT64_MAX);
        const auto limit=number(optional(args,"limit","100"),1000);
        // This explicit inspection also detects missing result rows or corrupt
        // coverage receipts, not just invalid public-key relations in this page.
        journal.check();
        const auto rows=journal.results(scope(args),int64_t(after),uint32_t(limit));
        std::cout<<"{\"results\":[";
        for(size_t i=0;i<rows.size();++i){const auto& row=rows[i];
            std::cout<<(i?",":"")<<"{\"id\":"<<quote(std::to_string(row.id))<<",\"block\":"<<quote(row.block.hex())
                <<",\"scalar\":"<<quote(row.scalar.hex())<<",\"target\":"<<row.target
                <<",\"target_bytes\":"<<quote(hex(row.target_bytes.data(),row.target_bytes.size()))<<'}';
        }
        std::cout<<"],\"next_after\":"<<quote(std::to_string(rows.empty()?after:uint64_t(rows.back().id)))<<"}";flush();return 0;
    }
    const auto grant=parse_grant(journal,required(args,"grant"));const auto mode=journal.manifest(grant.scope).mode;
    const auto device=decimal(optional(args,"device","0"),std::numeric_limits<int>::max());
    CheckpointOptions options;
    options.candidate_capacity=uint32_t(number(optional(args,"candidate-capacity","1024"),mode==Mode::XPoint?1048576:65536));
    options.checkpoint_seconds=uint32_t(decimal(optional(args,"checkpoint-seconds","10"),60));
    if(mode==Mode::XPoint){
        for(const auto* key:{"table","giant-batch","target-batch","group-size","host-memory","reserve-bytes"})
            if(args.count(key))throw std::invalid_argument(std::string("xpoint does not accept --")+key);
        options.xpoint_steps=uint64_t(number(optional(args,"batch-size","1048576"),1048576));
        const auto kernel=optional(args,"kernel","stepped");
        if(kernel!="stepped" && kernel!="direct")throw std::invalid_argument("kernel must be stepped or direct");
    }else{
        if(args.count("batch-size") || args.count("kernel"))throw std::invalid_argument("BSGS uses --giant-batch and --group-size");
        options.target_batch=uint32_t(number(optional(args,"target-batch","64"),64));
        options.giant_steps=uint64_t(number(optional(args,"giant-batch","16384"),1048576/options.target_batch));
        const auto group=optional(args,"group-size","auto");
        if(group!="auto" && group!="1" && group!="8")throw std::invalid_argument("group-size must be auto, 1 or 8");
    }
#ifndef KEYHUNT_HAS_GPU
    (void)device;
    (void)elapsed;
    discover_gpu();return 2; // Explicit backend availability; no CPU search fallback.
#else
    const auto selected = select_gpu(int(device));
    core::XPointVerifier verifier;CheckpointSummary summary;
    double preparation_ms=0,executor_setup_ms=0,table_upload_ms=0;
    const auto notify=[&](const std::vector<ScalarInterval>& coverage,size_t matches,double ms){
        std::cout<<"{\"type\":\"checkpoint\",\"durability\":\"local\",\"durable_results\":true,\"durable_coverage\":"
            <<(coverage.empty()?"false":"true")<<",\"intervals\":[";
        for(size_t i=0;i<coverage.size();++i)std::cout<<(i?",":"")<<"{\"begin\":"<<quote(coverage[i].begin().hex())
            <<",\"end_exclusive\":"<<quote(coverage[i].end().hex())<<'}';
        std::cout<<"],\"match_observations\":"<<matches<<",\"transaction_ms\":"<<ms<<'}';flush();
    };
    LocalCheckpointControl control(journal.state_directory(),grant,int(device),selected.visible_devices);
    // Construct/upload the executor lazily, after binding validation, integrity
    // checks, the exclusive owner guard and durable executor-generation allocation.
    if(mode==Mode::XPoint){
        const auto targets=core::XPointTargets::load(required(args,"targets"));
        XPointOptions gpu;gpu.max_steps=options.xpoint_steps;gpu.candidate_capacity=options.candidate_capacity;
        gpu.kernel=optional(args,"kernel","stepped")=="direct"?XPointKernel::Direct:XPointKernel::Stepped;
        std::unique_ptr<GpuXPointExecutor> executor;
        summary=CheckpointRun::xpoint(journal,grant,targets,verifier,[&](const auto& batch){
            if(!executor){
                const auto setup_start=elapsed();
                executor=std::make_unique<GpuXPointExecutor>(int(device),targets,verifier,gpu);
                executor_setup_ms=elapsed()-setup_start;preparation_ms=elapsed();
            }
            const auto ticket=executor->submit(batch);executor->drain();return executor->take(ticket);
        },options,notify,[&]{executor.reset();control.close();},control.callbacks());
    }else{
        const auto targets=core::BsgsPublicKeyTargets::load(required(args,"targets"));const auto table=load_table(args,targets);
        BsgsSearchOptions gpu;gpu.max_steps=options.giant_steps*options.target_batch;gpu.candidate_capacity=options.candidate_capacity;
        const auto group=optional(args,"group-size","auto");gpu.group_size=group=="auto"?0:unsigned(decimal(group));
        gpu.host_memory_bytes=decimal(optional(args,"host-memory","1073741824"));
        gpu.memory_reserve_bytes=decimal(optional(args,"reserve-bytes","67108864"));
        std::unique_ptr<GpuBsgsExecutor> executor;
        summary=CheckpointRun::bsgs(journal,grant,targets,table,verifier,[&](const auto& batch){
            if(!executor){
                const auto setup_start=elapsed();
                executor=std::make_unique<GpuBsgsExecutor>(int(device),table,targets,verifier,gpu);
                executor_setup_ms=elapsed()-setup_start;preparation_ms=elapsed();
                table_upload_ms=executor->table_upload_ms();
            }
            const auto ticket=executor->submit(batch);executor->drain();return executor->take(ticket);
        },options,notify,[&]{executor.reset();control.close();},control.callbacks());
    }
    std::cout<<"{\"type\":\"summary\",\"complete\":"<<(summary.complete?"true":"false")
        <<",\"durability\":\"local\",\"durable_coverage\":true"
        <<",\"resumed_scalars\":"<<quote(summary.resumed_scalars.hex())<<",\"computed_scalars\":"<<quote(summary.computed_scalars.hex())
        <<",\"device_steps\":"<<quote(summary.device_steps.hex())<<",\"match_observations\":"<<summary.match_observations
        <<",\"batches\":"<<summary.batches<<",\"overflow_replays\":"<<summary.overflows<<",\"checkpoints\":"<<summary.checkpoints
        <<",\"checkpoint_ms\":"<<summary.checkpoint_ms
        <<",\"metrics_version\":2,\"device\":"<<device<<",\"uuid\":"<<quote(selected.device.uuid)
        <<",\"mode\":"<<quote(mode==Mode::XPoint?"xpoint":"bsgs")
        <<",\"checkpoint_seconds\":"<<options.checkpoint_seconds<<",\"bsgs_group_size\":"<<summary.bsgs_group_size
        <<",\"verified_device_steps\":"<<quote(summary.verified_device_steps.hex())
        <<",\"kernel_ms\":"<<summary.kernel_ms<<",\"download_ms\":"<<summary.download_ms
        <<",\"seed_ms\":"<<summary.seed_ms<<",\"verification_ms\":"<<summary.verification_ms
        <<",\"revalidation_ms\":"<<summary.revalidation_ms<<",\"executor_wall_ms\":"<<summary.executor_wall_ms
        <<",\"replay_kernel_ms\":"<<summary.replay_kernel_ms<<",\"download_bytes\":"<<quote(summary.download_bytes.hex())
        <<",\"peak_device_allocation_bytes\":"<<summary.peak_device_allocation_bytes
        <<",\"peak_pinned_allocation_bytes\":"<<summary.peak_pinned_allocation_bytes
        <<",\"preparation_ms\":"<<preparation_ms<<",\"executor_setup_ms\":"<<executor_setup_ms
        <<",\"table_upload_ms\":"<<table_upload_ms<<",\"wall_ms\":"<<elapsed()
        <<",\"bsgs_groups\":[";
    bool comma=false;
    for(unsigned i=0;i<summary.bsgs_groups.size();++i){
        const auto& group=summary.bsgs_groups[i];if(!group.batches)continue;
        std::cout<<(comma?",":"")<<"{\"group_size\":"<<(i?8:1)<<",\"batches\":"<<group.batches
            <<",\"overflow_replays\":"<<group.overflows<<",\"device_steps\":"<<quote(group.device_steps.hex())
            <<",\"verified_device_steps\":"<<quote(group.verified_device_steps.hex())<<",\"kernel_ms\":"<<group.kernel_ms<<'}';
        comma=true;
    }
    std::cout<<"]}";flush();return 0;
#endif
}
} // namespace keyhunt::backend
