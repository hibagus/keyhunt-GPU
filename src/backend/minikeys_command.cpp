#include "keyhunt/backend/device.h"
#include "keyhunt/core/minikey_search.h"
#include "keyhunt/scheduler/xpoint_batch_size.h"
#ifdef KEYHUNT_HAS_GPU
#include "keyhunt/backend/gpu_minikeys.h"
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
    if (!std::cout) throw std::runtime_error("failed to write minikey output; no further batches submitted");
}
#endif
}
int minikeys_command(int argc, char** argv) {
    const char* usage = "usage: keyhunt minikeys --length 22|30 --backend hip|cuda --range START:END --targets FILE [--encoding compressed|uncompressed|both] [--device N] [--batch-size 1..1048576] [--candidate-capacity 1..1048576] [--input-format address|hash160] [--kernel direct] (END is exclusive; NDJSON output)";
    // Inspection is CPU-only and accepts a public candidate even when its check
    // byte fails: operators can obtain an exact range start without searching.
    if(argc>=3 && std::string(argv[2])=="inspect"){
        if(argc!=5 || std::string(argv[3])!="--key")throw std::invalid_argument("usage: keyhunt minikeys inspect --key TEXT");
        const std::string text=argv[4];const auto ordinal=core::minikey_ordinal(text);const auto scalar=core::minikey_scalar(text);
        std::cout<<"{\"coordinate_space\":\"minikey-ordinal-v1\",\"length\":"<<text.size()<<",\"ordinal\":\""<<ordinal.hex()
                 <<"\",\"valid\":"<<(scalar?"true":"false");
        if(scalar)std::cout<<",\"scalar\":\""<<scalar->hex()<<'"';
        std::cout<<",\"space_end_exclusive\":\""<<core::minikey_space_end(text.size()).hex()<<"\"}\n";return 0;
    }
    std::map<std::string,std::string> args;
    for (int i=2;i<argc;i+=2) {
        if (i+1 == argc) throw std::invalid_argument(usage);
        const std::string key = argv[i];
        if (key != "--backend" && key != "--range" && key != "--targets" && key != "--device" &&
            key != "--length" && key != "--input-format" && key != "--encoding" && key != "--batch-size" && key != "--candidate-capacity" && key != "--kernel") throw std::invalid_argument(usage);
        if (!args.emplace(key,argv[i+1]).second) throw std::invalid_argument("duplicate minikey option: "+key);
    }
    if ((args["--backend"] != "hip" && args["--backend"] != "cuda") || args["--range"].empty() || args["--targets"].empty())
        throw std::invalid_argument(usage);
    require_backend(args["--backend"]);
    const auto range = args["--range"];
    const auto colon = range.find(':');
    if (colon == std::string::npos) throw std::invalid_argument(usage);
    using core::UInt256;
    const core::ScalarInterval interval(UInt256::from_hex(range.substr(0,colon)),UInt256::from_hex(range.substr(colon+1)));
    const uint64_t device = args.count("--device") ? decimal(args["--device"]) : 0;
    const uint64_t batch_size = args.count("--batch-size") ? decimal(args["--batch-size"]) : 1048576;
    const uint64_t capacity = args.count("--candidate-capacity") ? decimal(args["--candidate-capacity"]) : 1024;
    if (device > std::numeric_limits<int>::max() || !batch_size || batch_size > 1048576 || !capacity || capacity > 1048576)
        throw std::invalid_argument(usage);
    const auto encoding = core::hash160_encoding(args.count("--encoding") ? args["--encoding"] : "both");
    const std::string kernel=args.count("--kernel")?args["--kernel"]:"direct";
    if(kernel!="direct")throw std::invalid_argument("minikeys supports only the direct kernel");
    const auto length=decimal(args["--length"]);if(length!=22&&length!=30)throw std::invalid_argument("minikey length must be 22 or 30");
    if(interval.end()>core::minikey_space_end(unsigned(length)))throw std::invalid_argument("range exceeds minikey ordinal space");
    const auto input=args.count("--input-format")?args["--input-format"]:"address";
    if(input!="address"&&input!="hash160")throw std::invalid_argument("input-format must be address or hash160");
#ifndef KEYHUNT_HAS_GPU
    (void)encoding;
    discover_gpu(); // explicit error; a GPU request never falls back to CPU
    return 2;
#else
    const auto wall_start = std::chrono::steady_clock::now();
    const auto targets=core::MinikeyTargets::load(args["--targets"],unsigned(length),
        input=="address"?core::Hash160Input::BitcoinAddress:core::Hash160Input::Hex,encoding);
    const auto selected = select_gpu(int(device));
    core::XPointVerifier verifier;
    MinikeysOptions options;
    options.max_steps = batch_size; options.candidate_capacity = uint32_t(capacity);
    GpuMinikeysExecutor executor(int(device),targets,verifier,options);
    const double preparation_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count();
    // This standalone owner uses one lazy block, preserving C05 exact arithmetic.
    // Assignment IDs here are ephemeral; durable execution uses checkpoint commands.
    scheduler::BlockGrid grid(interval,interval.size());
    scheduler::ExecutionIdentity identity;
    identity.target_digest = targets.digest();
    identity.algorithm = scheduler::WorkAlgorithm::DirectMinikeysV1;
    identity.assignment_id[0] = 1;
    identity.assignment_generation = identity.executor_generation = 1;
    std::cout << std::setprecision(9) << "{\"type\":\"start\",\"backend\":\"" << gpu_backend_name() << "\",\"mode\":\"minikeys\",\"coordinate_space\":\"minikey-ordinal-v1\",\"device\":" << device
              << ",\"uuid\":\"" << selected.device.uuid << "\",\"target_count\":" << targets.values().size()
              << ",\"target_digest\":\"" << hex_bytes(targets.digest().data(),targets.digest().size())
              << "\",\"begin\":\"" << interval.begin().hex() << "\",\"end_exclusive\":\"" << interval.end().hex()
              << "\",\"kernel\":\"" << kernel << "\",\"durable_coverage\":false,\"preparation_ms\":" << preparation_ms << '}';
    flush_record();
    auto cursor = interval.begin();
    UInt256 verified, attempts, match_count;
    uint64_t launches = 0, overflows = 0;
    scheduler::XPointBatchSize sizing(batch_size,uint32_t(capacity),targets.max_matches_per_scalar());
    double kernel_ms = 0, download_ms = 0, verification_ms = 0, seed_ms = 0;
    while (auto work = scheduler::WorkUnit::plan(grid,UInt256(0),cursor,batch_size,identity)) {
        while (auto batch = scheduler::KernelBatch::plan(*work,cursor,sizing.limit())) {
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
                const auto text=core::minikey_text(match.scalar,targets.length());const auto scalar=core::minikey_scalar(text);
                if(!scalar)throw std::logic_error("verified minikey became invalid");
                std::cout<<(i?",":"")<<"{\"ordinal\":\""<<match.scalar.hex()<<"\",\"minikey\":\""<<text
                         <<"\",\"scalar\":\""<<scalar->hex()<<"\",\"hash160\":\""
                         <<hex_bytes(targets.values()[match.target].data()+2,20)<<"\",\"encoding\":\""
                         <<core::hash160_encoding_name(targets.values()[match.target][1])<<"\",\"target\":"<<match.target<<'}';
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
            cursor = batch->interval().end();
        }
    }
    if (verified != interval.size()) throw std::logic_error("minikey ordinal interval is incomplete");
    const double wall_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count();
    std::cout << "{\"type\":\"summary\",\"coordinate_space\":\"minikey-ordinal-v1\",\"complete\":true,\"durable_coverage\":false,\"verified_steps\":\"" << verified.hex()
              << "\",\"device_steps\":\"" << attempts.hex() << "\",\"matches\":\"" << match_count.hex()
              << "\",\"launch_count\":" << launches << ",\"overflow_replays\":" << overflows
              << ",\"kernel_ms\":" << kernel_ms << ",\"download_ms\":" << download_ms
              << ",\"verification_ms\":" << verification_ms << ",\"seed_ms\":" << seed_ms << ",\"wall_ms\":" << wall_ms << '}';
    flush_record();
    return 0;
#endif
}
} // namespace keyhunt::backend
