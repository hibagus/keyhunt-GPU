#include "keyhunt/scheduler/scalar_batch_planner.h"
#include "keyhunt/backend/device.h"
#include "keyhunt/core/xpoint_search.h"
#include "keyhunt/scheduler/xpoint_batch_size.h"
#ifdef KEYHUNT_HAS_GPU
#include "keyhunt/backend/gpu_xpoint.h"
#endif
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
namespace {
uint64_t decimal(const std::string& value) {
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(),value.data()+value.size(),result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size())
        throw std::invalid_argument("expected an unsigned decimal integer: "+value);
    return result;
}
#ifdef KEYHUNT_HAS_GPU
std::string hex_bytes(const uint8_t* bytes, size_t count) {
    const char* digits = "0123456789abcdef";
    std::string result;
    for (size_t i=0;i<count;++i) { result += digits[bytes[i]>>4]; result += digits[bytes[i]&15]; }
    return result;
}
void flush_record() {
    std::cout << '\n' << std::flush;
    if (!std::cout) throw std::runtime_error("failed to write xpoint output; no further batches submitted");
}
#endif
}
int xpoint_command(int argc, char** argv) {
    const char* usage = "usage: keyhunt xpoint --backend hip|cuda --range START:END --targets FILE [--device N] [--batch-size 1..1048576] [--candidate-capacity 1..1048576] [--kernel stepped|direct|glv] [--stride HEX] [--order forward|reverse] [--batch-order forward|both-ends|dance] [--endomorphism none|orbit] (END is exclusive; NDJSON output)";
    std::map<std::string,std::string> args;
    for (int i=2;i<argc;i+=2) {
        if (i+1 == argc) throw std::invalid_argument(usage);
        const std::string key = argv[i];
        if (key != "--backend" && key != "--range" && key != "--targets" && key != "--device" &&
            key != "--batch-size" && key != "--candidate-capacity" && key != "--kernel" && key != "--stride" && key != "--batch-order" && key != "--order" && key != "--endomorphism") throw std::invalid_argument(usage);
        if (!args.emplace(key,argv[i+1]).second) throw std::invalid_argument("duplicate xpoint option: "+key);
    }
    if ((args["--backend"] != "hip" && args["--backend"] != "cuda") || args["--range"].empty() || args["--targets"].empty())
        throw std::invalid_argument(usage);
    require_backend(args["--backend"]);
    const auto range = args["--range"];
    const auto colon = range.find(':');
    if (colon == std::string::npos) throw std::invalid_argument(usage);
    using core::UInt256;
    const core::ScalarInterval scalar_range(UInt256::from_hex(range.substr(0,colon)),UInt256::from_hex(range.substr(colon+1)));
    const auto stride=UInt256::from_hex(args.count("--stride")?args["--stride"]:"1");
    core::validate_scalar_stride(stride);
    const auto order=args.count("--order")?args["--order"]:"forward";
    if(order!="forward" && order!="reverse")throw std::invalid_argument("order must be forward or reverse");
    const bool reverse=order=="reverse";
    const auto batch_order=scheduler::parse_scalar_batch_order(args.count("--batch-order")?args["--batch-order"]:"forward");
    const auto endomorphism=args.count("--endomorphism")?args["--endomorphism"]:"none";
    if(endomorphism!="none" && endomorphism!="orbit")throw std::invalid_argument("endomorphism must be none or orbit");
    const bool orbit=endomorphism=="orbit";
    const auto mapping=stride==UInt256(1) && !reverse && !orbit?std::optional<core::ScalarStride>{}:std::make_optional(core::ScalarStride(scalar_range,stride,reverse,orbit));
    const auto interval=mapping?mapping->indices():scalar_range;
    const uint64_t device = args.count("--device") ? decimal(args["--device"]) : 0;
    const uint64_t batch_size = args.count("--batch-size") ? decimal(args["--batch-size"]) : 1048576;
    const uint64_t capacity = args.count("--candidate-capacity") ? decimal(args["--candidate-capacity"]) : 1024;
    if (device > std::numeric_limits<int>::max() || !batch_size || batch_size > 1048576 || !capacity || capacity > 1048576)
        throw std::invalid_argument(usage);
    const std::string kernel = args.count("--kernel") ? args["--kernel"] : "stepped";
    if (kernel != "direct" && kernel != "stepped" && kernel != "glv") throw std::invalid_argument(usage);
#ifndef KEYHUNT_HAS_GPU
    discover_gpu(); // explicit error; a GPU request never falls back to CPU
    return 2;
#else
    const auto wall_start = std::chrono::steady_clock::now();
    const auto targets = core::XPointTargets::load(args["--targets"]);
    const auto selected = select_gpu(int(device));
    core::XPointVerifier verifier;
    XPointOptions options;
    options.stride = stride;options.reverse=reverse;options.orbit=orbit;
    options.max_steps = batch_size; options.candidate_capacity = uint32_t(capacity);
    options.kernel = scalar_search_kernel(kernel);
    GpuXPointExecutor executor(int(device),targets,verifier,options);
    const double preparation_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count();
    // This standalone owner uses one lazy block, preserving C05 exact arithmetic.
    // Assignment IDs are ephemeral; durable ownership/checkpointing arrives in C12/13.
    scheduler::BlockGrid grid(interval,interval.size());
    scheduler::ExecutionIdentity identity;
    identity.target_digest = targets.digest();
    if(mapping){identity.algorithm=scheduler::strided_algorithm(identity.algorithm,reverse,orbit);identity.stride_mapping=mapping;}
    identity.assignment_id[0] = 1;
    identity.assignment_generation = identity.executor_generation = 1;
    std::cout << std::setprecision(9) << "{\"type\":\"start\",\"backend\":\"" << gpu_backend_name() << "\",\"mode\":\"xpoint\",\"device\":" << device
              << ",\"uuid\":\"" << selected.device.uuid << "\",\"target_count\":" << targets.values().size()
              << ",\"target_digest\":\"" << hex_bytes(targets.digest().data(),targets.digest().size())
              << "\",\"begin\":\"" << interval.begin().hex() << "\",\"end_exclusive\":\"" << interval.end().hex()
              << "\",\"kernel\":\"" << kernel << "\",\"durable_coverage\":false,\"preparation_ms\":" << preparation_ms;
    if(mapping)std::cout<<",\"coordinate_space\":\""<<mapping->coordinate_space()<<"\",\"scalar_begin\":\""<<scalar_range.begin().hex()
        <<"\",\"scalar_end_exclusive\":\""<<scalar_range.end().hex()<<"\",\"stride\":\""<<stride.hex()<<'"';
    if(orbit)std::cout<<",\"endomorphism\":\"orbit\",\"seed_count\":\""<<mapping->seed_count().hex()<<'"';
    std::cout<<",\"batch_order\":\""<<scheduler::scalar_batch_order_name(batch_order)<<"\"}";
    flush_record();
    scheduler::ScalarBatchPlanner planner(grid,UInt256(),{interval},identity,batch_order);
    UInt256 verified, attempts, match_count;
    uint64_t launches = 0, overflows = 0;
    scheduler::XPointBatchSize sizing(batch_size,uint32_t(capacity));
    double kernel_ms = 0, download_ms = 0, verification_ms = 0, seed_ms = 0;
    while (const auto selected_batch=planner.plan(UInt256(batch_size),sizing.limit())) {
        const auto* batch=&selected_batch->batch;
        const auto ticket = executor.submit(*batch);
        executor.drain(); // only this stream; executor API also supports poll()
        const auto result = executor.take(ticket);
        ++launches;
        attempts = attempts.add(UInt256(result.device_steps));
        seed_ms += result.seed_ms; kernel_ms += result.kernel_ms; download_ms += result.download_ms; verification_ms += result.verification_ms;
        std::cout << "{\"type\":\"batch\",\"begin\":\"" << batch->interval().begin().hex()
                  << "\",\"end_exclusive\":\"" << batch->interval().end().hex()
                  << "\",\"overflow\":" << (result.overflow ? "true" : "false")
                  << ",\"verified_steps\":" << result.verified_steps << ",\"device_steps\":" << result.device_steps
                  << ",\"candidate_count\":" << result.candidate_count << ",\"kernel_ms\":" << result.kernel_ms
                  << ",\"download_ms\":" << result.download_ms << ",\"verification_ms\":" << result.verification_ms
                  << ",\"seed_ms\":" << result.seed_ms << ",\"wall_ms\":" << result.wall_ms << ",\"device_allocation_bytes\":" << result.device_allocation_bytes
                  << ",\"pinned_allocation_bytes\":" << result.pinned_allocation_bytes
                  << ",\"download_bytes\":" << result.download_bytes << ",\"matches\":[";
        for (size_t i=0;i<result.matches.size();++i) {
            const auto& match = result.matches[i];
            std::cout << (i ? "," : "") << "{\"scalar\":\"" << (mapping?mapping->scalar(match.scalar):match.scalar).hex() << "\",\"x\":\""
                      << hex_bytes(targets.values()[match.target].data(),32) << "\",\"target\":" << match.target ;
            if(mapping)std::cout<<",\"candidate_index\":\""<<match.scalar.hex()<<'"';
            if(orbit)std::cout<<",\"seed_scalar\":\""<<mapping->seed(match.scalar).hex()
                <<"\",\"orbit_variant\":"<<mapping->variant(match.scalar);
            std::cout<<'}';
        }
        std::cout << "]}";
        flush_record(); // output backpressure precedes any cursor advancement
        if (result.overflow) {
            ++overflows;
            sizing.overflow(batch->step_count());
            continue;
        }
        sizing.accepted(result.candidate_count);
        verified = verified.add(UInt256(result.verified_steps));
        match_count = match_count.add(UInt256(result.matches.size()));
        planner.accept();
    }
    if (verified != interval.size()) throw std::logic_error("xpoint verified interval is incomplete");
    const double wall_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count();
    std::cout << "{\"type\":\"summary\",\"complete\":true,\"durable_coverage\":false,\"verified_steps\":\"" << verified.hex()
              << "\",\"device_steps\":\"" << attempts.hex() << "\",\"matches\":\"" << match_count.hex()
              << "\",\"launch_count\":" << launches << ",\"overflow_replays\":" << overflows
              << ",\"kernel_ms\":" << kernel_ms << ",\"download_ms\":" << download_ms
              << ",\"verification_ms\":" << verification_ms << ",\"seed_ms\":" << seed_ms << ",\"wall_ms\":" << wall_ms ;
    if(mapping)std::cout<<",\"coordinate_space\":\""<<mapping->coordinate_space()<<'"';
    std::cout<<",\"batch_order\":\""<<scheduler::scalar_batch_order_name(batch_order)<<"\"}";
    flush_record();
    return 0;
#endif
}
} // namespace keyhunt::backend
