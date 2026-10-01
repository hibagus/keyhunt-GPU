#include "keyhunt/backend/device.h"
#include "keyhunt/core/bsgs_search.h"
#ifdef KEYHUNT_HAS_GPU
#include "keyhunt/backend/gpu_bsgs.h"
#endif
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

namespace keyhunt::backend {
namespace {
uint64_t decimal(const std::string& value) {
    uint64_t result=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) throw std::invalid_argument("expected unsigned decimal: "+value);
    return result;
}
#ifdef KEYHUNT_HAS_GPU
std::string hex(const uint8_t* bytes,size_t count) {
    const char* digits="0123456789abcdef"; std::string result;
    for(size_t i=0;i<count;++i){result+=digits[bytes[i]>>4];result+=digits[bytes[i]&15];} return result;
}
void flush_record() {
    std::cout<<'\n'<<std::flush;
    if (!std::cout) throw std::runtime_error("failed to write BSGS output; no further work submitted");
}
#endif
}
int bsgs_command(int argc,char** argv) {
    const char* usage="usage: keyhunt bsgs --backend hip|cuda --range START:END --targets FILE --table FILE [--device N] [--giant-batch 1..1048576] [--target-batch 1..64] [--candidate-capacity 1..65536] [--group-size auto|1|8] [--tile-order forward|reverse|both-ends|dance] [--host-memory BYTES] [--reserve-bytes BYTES] (END exclusive; NDJSON output)";
    std::map<std::string,std::string> args;
    for(int i=2;i<argc;i+=2) {
        if(i+1==argc) throw std::invalid_argument(usage);
        const std::string key=argv[i];
        if(key!="--backend" && key!="--range" && key!="--targets" && key!="--table" && key!="--device" &&
           key!="--giant-batch" && key!="--target-batch" && key!="--candidate-capacity" && key!="--group-size" &&
           key!="--host-memory" && key!="--reserve-bytes" && key!="--tile-order") throw std::invalid_argument(usage);
        if(!args.emplace(key,argv[i+1]).second) throw std::invalid_argument("duplicate BSGS option: "+key);
    }
    if((args["--backend"]!="hip" && args["--backend"]!="cuda") || args["--range"].empty() || args["--targets"].empty() || args["--table"].empty())
        throw std::invalid_argument(usage);
    const auto tile_order=args.count("--tile-order")?args["--tile-order"]:"forward";
    const auto order=core::parse_bsgs_tile_order(tile_order);
    require_backend(args["--backend"]);
    const auto range=args["--range"]; const auto colon=range.find(':');
    if(colon==std::string::npos) throw std::invalid_argument(usage);
    using core::UInt256;
    const core::ScalarInterval interval(UInt256::from_hex(range.substr(0,colon)),UInt256::from_hex(range.substr(colon+1)));
    const auto option=[&](const char* name,uint64_t fallback){return args.count(name)?decimal(args[name]):fallback;};
    const uint64_t device=option("--device",0),giants=option("--giant-batch",16384),target_batch=option("--target-batch",64),
        capacity=option("--candidate-capacity",1024),
        host_memory=option("--host-memory",1024ULL*1024*1024),reserve=option("--reserve-bytes",64*1024*1024);
    const uint64_t group=(!args.count("--group-size") || args["--group-size"]=="auto")?0:decimal(args["--group-size"]);
    if(args.count("--group-size") && args["--group-size"]!="auto" && !group) throw std::invalid_argument(usage);
    if(device>std::numeric_limits<int>::max() || !target_batch || target_batch>64 || !giants || giants>1048576/target_batch ||
       !capacity || capacity>65536 || (group!=0 && group!=1 && group!=8) || !host_memory) throw std::invalid_argument(usage);
#ifndef KEYHUNT_HAS_GPU
    (void)reserve;(void)order;
    discover_gpu(); // a GPU request never silently falls back to CPU
    return 2;
#else
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    const auto targets=core::BsgsPublicKeyTargets::load(args["--targets"]);
    // Target storage coexists with cache decoding. Reserve it before asking
    // the table loader to allocate its checked peak buffers.
    const uint64_t target_bytes=targets.values().capacity()*sizeof(core::UncompressedPublicKey);
    if (target_bytes>=host_memory) throw std::invalid_argument("BSGS targets exceed host memory budget");
    const auto table=bsgs::Table::load(args["--table"],{16,host_memory-target_bytes});
    const auto selected = select_gpu(int(device));
    core::XPointVerifier verifier;
    BsgsSearchOptions options;
    options.max_steps=giants*target_batch; options.candidate_capacity=uint32_t(capacity); options.group_size=unsigned(group);
    options.host_memory_bytes=host_memory; options.memory_reserve_bytes=reserve;
    GpuBsgsExecutor executor(int(device),table,targets,verifier,options);
    const auto elapsed=[&]{return std::chrono::duration<double,std::milli>(Clock::now()-start).count();};
    std::cout<<std::setprecision(9)<<"{\"type\":\"start\",\"backend\":\"" << gpu_backend_name() << "\",\"mode\":\"bsgs\",\"device\":"<<device
        <<",\"uuid\":\""<<selected.device.uuid<<"\",\"m\":"<<table.memory().m
        <<",\"table_checksum\":\""<<hex(table.checksum().data(),32)<<"\",\"target_digest\":\""<<hex(targets.digest().data(),32)
        <<"\",\"target_count\":"<<targets.values().size()<<",\"begin\":\""<<interval.begin().hex()<<"\",\"end_exclusive\":\""<<interval.end().hex()
        <<"\",\"group_size\":"<<group<<",\"tile_order\":"<<std::quoted(tile_order)<<",\"durable_coverage\":false,\"preparation_ms\":"<<elapsed()
        <<",\"table_upload_ms\":"<<executor.table_upload_ms()<<'}';
    flush_record();
    core::BsgsTilePlanner planner({interval},table.memory().m,giants,order);
    const auto tile_width=UInt256(table.memory().m).multiply(UInt256(giants));
    UInt256 verified_scalars,verified_steps,device_steps,match_count;
    uint64_t launches=0,overflows=0,tiles=0;
    double kernel_ms=0,download_ms=0,verification_ms=0,seed_ms=0;
    while(const auto selected_tile=planner.next(tile_width)) {
        const auto& tile=selected_tile->interval;
        // Advance to the other end only after every target subset of this tile,
        // including overflow retries, has completed and its receipt is emitted.
        uint32_t first=0,limit=uint32_t(target_batch);
        uint64_t tile_steps=0;
        while(first<targets.values().size()) {
            const uint32_t count=uint32_t(std::min<size_t>(limit,targets.values().size()-first));
            const core::BsgsBatch batch(tile,table.memory().m,first,count,targets.digest(),table.checksum());
            const auto ticket=executor.submit(batch); executor.drain(); const auto result=executor.take(ticket);
            ++launches; device_steps=device_steps.add(UInt256(result.device_steps));
            kernel_ms+=result.kernel_ms; download_ms+=result.download_ms; verification_ms+=result.verification_ms; seed_ms+=result.seed_ms;
            std::cout<<"{\"type\":\"batch\",\"begin\":\""<<tile.begin().hex()<<"\",\"end_exclusive\":\""<<tile.end().hex()
                <<"\",\"first_target\":"<<first<<",\"target_count\":"<<count<<",\"giants_per_target\":"<<batch.giants()
                <<",\"group_size\":"<<result.group_size<<",\"overflow\":"<<(result.overflow?"true":"false")<<",\"device_steps\":"<<result.device_steps
                <<",\"verified_steps\":"<<result.verified_steps<<",\"candidate_count\":"<<result.candidate_count
                <<",\"tail_rejections\":"<<result.tail_rejections<<",\"kernel_ms\":"<<result.kernel_ms
                <<",\"download_ms\":"<<result.download_ms<<",\"verification_ms\":"<<result.verification_ms
                <<",\"seed_ms\":"<<result.seed_ms<<",\"wall_ms\":"<<result.wall_ms
                <<",\"device_allocation_bytes\":"<<result.device_allocation_bytes<<",\"pinned_allocation_bytes\":"<<result.pinned_allocation_bytes
                <<",\"download_bytes\":"<<result.download_bytes<<",\"matches\":[";
            for(size_t i=0;i<result.matches.size();++i) {
                const auto& match=result.matches[i];
                std::cout<<(i?",":"")<<"{\"scalar\":\""<<match.scalar.hex()<<"\",\"target\":"<<match.target
                    <<",\"public_key\":\""<<hex(targets.values()[match.target].data(),65)<<"\"}";
            }
            std::cout<<"]}"; flush_record(); // output backpressure precedes any receipt advancement
            if(result.overflow) {
                ++overflows;
                if(count==1) throw std::logic_error("single canonical BSGS target cannot overflow");
                limit=uint32_t(std::min<uint64_t>(capacity,count/2));
                continue; // discard the whole attempt; replay the same target cursor
            }
            first+=count; tile_steps+=result.verified_steps;
            verified_steps=verified_steps.add(UInt256(result.verified_steps));
            match_count=match_count.add(UInt256(result.matches.size()));
        }
        // All target subsets completed. Credit the scalar tile once, independent
        // of target count; target giant steps remain a separate work metric.
        std::cout<<"{\"type\":\"tile\",\"begin\":\""<<tile.begin().hex()<<"\",\"end_exclusive\":\""<<tile.end().hex()
            <<"\",\"targets_completed\":"<<first<<",\"verified_target_steps\":"<<tile_steps<<",\"durable_coverage\":false}";
        flush_record();
        verified_scalars=verified_scalars.add(tile.size()); ++tiles;
    }
    if(verified_scalars!=interval.size()) throw std::logic_error("BSGS interval incomplete");
    std::cout<<"{\"type\":\"summary\",\"complete\":true,\"durable_coverage\":false,\"verified_scalars\":\""<<verified_scalars.hex()
        <<"\",\"verified_target_steps\":\""<<verified_steps.hex()<<"\",\"device_steps\":\""<<device_steps.hex()
        <<"\",\"matches\":\""<<match_count.hex()<<"\",\"launch_count\":"<<launches<<",\"overflow_replays\":"<<overflows<<",\"tile_order\":"<<std::quoted(tile_order)<<",\"tiles\":"<<tiles
        <<",\"kernel_ms\":"<<kernel_ms<<",\"download_ms\":"<<download_ms<<",\"verification_ms\":"<<verification_ms
        <<",\"seed_ms\":"<<seed_ms<<",\"wall_ms\":"<<elapsed()<<'}';
    flush_record(); return 0;
#endif
}
} // namespace keyhunt::backend
