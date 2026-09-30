// Checked preparation/lookup diagnostics only. These timings are not giant-step
// search throughput: probes include counters, downloads and CPU comparisons.
#include "keyhunt/backend/hip_bsgs_table.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace keyhunt;
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
int main(int argc,char** argv) {
    try {
        if (argc>3) throw std::invalid_argument("usage: hip_bsgs_benchmark [device] [m]");
        const int device=argc>1 ? std::stoi(argv[1]) : 0;
        const uint64_t m=argc>2 ? std::stoull(argv[2]) : 65537;
        if (m<2 || m>1048576) throw std::invalid_argument("benchmark m must be in [2,1048576]");
        const auto start=Clock::now();
        const auto table=bsgs::Table::build(m);
        const double build_ms=elapsed(start);
        const auto preparing=Clock::now();
        backend::BsgsUploadOptions options; options.max_queries=4096;
        backend::HipBsgsTable owner(device,table,options);
        const double prepare_ms=elapsed(preparing);
        std::vector<bsgs::Key> keys[2];
        std::mt19937_64 random(0xC10);
        // Sample across the resident table instead of selecting adjacent buckets.
        // Repeated queries are allowed; the exact deterministic corpus is reported.
        const auto query_count=std::min<uint64_t>(4096,m-1);
        while (keys[0].size()<query_count) {
            const auto& entry=table.entries()[random()%m];
            if (!entry.j) continue;
            keys[0].push_back(entry.key);
            auto negative=entry.key; negative.bytes[0]^=1; keys[1].push_back(negative);
        }
        std::cout<<std::setprecision(9)<<"{\"device\":"<<device<<",\"m\":"<<m<<",\"seed\":3088,\"cpu_build_ms\":"<<build_ms
            <<",\"prepare_wall_ms\":"<<prepare_ms<<",\"prepare_upload_ms\":"<<owner.preparation_upload_ms()
            <<",\"resident_bytes\":"<<table.memory().resident_bytes<<",\"host_peak_bytes\":"<<table.memory().host_peak_bytes
            <<",\"device_bytes\":"<<owner.device_bytes()<<",\"pinned_bytes\":"<<owner.pinned_bytes()<<",\"samples\":[";
        bool first=true;
        for (int sample=-1;sample<5;++sample) {
            for (unsigned order=0;order<2;++order) {
                const unsigned kind=sample>=0 && sample%2 ? 1-order : order;
                const auto result=owner.probe(keys[kind]);
                uint64_t hits=0,bloom_positive=0;
                for (const auto& hit:result.hits) { hits+=hit.end-hit.begin; bloom_positive+=hit.bloom_positive; }
                if (result.device_queries!=keys[kind].size() || hits!=(kind ? 0 : keys[kind].size()))
                    throw std::runtime_error("benchmark query results differ from expected baby/sign set");
                if (sample<0) continue;
                std::cout<<(first ? "" : ",")<<"{\"kind\":\""<<(kind ? "negative" : "positive")<<"\",\"sample\":"<<sample
                    <<",\"queries\":"<<result.device_queries<<",\"hits\":"<<hits<<",\"bloom_positive\":"<<bloom_positive
                    <<",\"upload_ms\":"<<result.upload_ms<<",\"kernel_ms\":"<<result.kernel_ms<<",\"download_ms\":"<<result.download_ms
                    <<",\"wall_ms\":"<<result.wall_ms<<'}';
                first=false;
            }
        }
        std::cout<<"]}\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
