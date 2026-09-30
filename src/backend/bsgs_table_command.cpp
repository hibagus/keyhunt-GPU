#include "keyhunt/core/bsgs_table.h"
#include "keyhunt/backend/device.h"
#ifdef KEYHUNT_HAS_HIP
#include "keyhunt/backend/hip_bsgs_table.h"
#endif
#include <limits>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
namespace {
uint64_t number(const std::string& text) {
    uint64_t value=0;
    const auto r=std::from_chars(text.data(),text.data()+text.size(),value);
    if (r.ec!=std::errc{} || r.ptr!=text.data()+text.size()) throw std::invalid_argument("expected an unsigned decimal integer: "+text);
    return value;
}
}
int bsgs_table_command(int argc,char** argv) {
    const char* usage="usage: keyhunt bsgs-table build --m N --output FILE [--bits-per-entry 8|16|32] [--host-memory BYTES] OR bsgs-table inspect --input FILE [--host-memory BYTES] OR bsgs-table validate --backend hip --input FILE [--device N] [--max-queries 1..65536] [--reserve-bytes BYTES] [--host-memory BYTES]";
    if (argc<3) throw std::invalid_argument(usage);
    const std::string action=argv[2];
    if (action!="build" && action!="inspect" && action!="validate") throw std::invalid_argument(usage);
    std::map<std::string,std::string> args;
    for (int i=3;i<argc;i+=2) {
        if (i+1>=argc) throw std::invalid_argument(usage);
        const std::string key=argv[i];
        if (key!="--host-memory" && !(action=="build" && (key=="--m" || key=="--output" || key=="--bits-per-entry")) &&
            !((action=="inspect" || action=="validate") && key=="--input") &&
            !(action=="validate" && (key=="--backend" || key=="--device" || key=="--max-queries" || key=="--reserve-bytes"))) throw std::invalid_argument(usage);
        if (!args.emplace(key,argv[i+1]).second) throw std::invalid_argument("duplicate BSGS option");
    }
    bsgs::Options options;
    if (args.count("--host-memory")) options.host_memory_bytes=number(args["--host-memory"]);
    if (args.count("--bits-per-entry")) {
        const auto value=number(args["--bits-per-entry"]);
        if (value>32) throw std::invalid_argument(usage);
        options.bits_per_entry=uint32_t(value);
    }
    if (action=="validate" && args["--backend"]!="hip") throw std::invalid_argument(usage);
    const auto device=args.count("--device") ? number(args["--device"]) : 0;
    const auto queries=args.count("--max-queries") ? number(args["--max-queries"]) : 4096;
    const auto reserve=args.count("--reserve-bytes") ? number(args["--reserve-bytes"]) : 64*1024*1024;
    if (device>uint64_t(std::numeric_limits<int>::max()) || !queries || queries>65536) throw std::invalid_argument(usage);
#ifndef KEYHUNT_HAS_HIP
    (void)reserve;
    if (action=="validate") discover_hip(); // no silent CPU substitute
#endif
    const auto start=std::chrono::steady_clock::now();
    auto table=[&] {
        if (action=="build") {
            if (!args.count("--m") || !args.count("--output") || args["--output"].empty()) throw std::invalid_argument(usage);
            auto built=bsgs::Table::build(number(args["--m"]),options);
            built.save(args["--output"]);
            return built;
        } else {
            if (!args.count("--input") || args["--input"].empty()) throw std::invalid_argument(usage);
            return bsgs::Table::load(args["--input"],options);
        }
    }();
    std::string device_uuid;
    uint64_t checked=0,device_bytes=0,pinned_bytes=0;
    double kernel_ms=0,upload_ms=0,download_ms=0,preparation_ms=0;
#ifdef KEYHUNT_HAS_HIP
    if (action=="validate") {
        const auto inventory=discover_hip();
        if (device>=inventory.devices.size()) throw std::invalid_argument("HIP device ordinal is not visible");
        device_uuid=inventory.devices[device].uuid;
        BsgsUploadOptions config; config.max_queries=uint32_t(queries); config.memory_reserve_bytes=reserve;
        config.host_memory_bytes=options.host_memory_bytes;
        HipBsgsTable prepared(int(device),table,config);
        device_bytes=prepared.device_bytes(); pinned_bytes=prepared.pinned_bytes();
        preparation_ms=prepared.preparation_upload_ms();
        std::vector<bsgs::Key> batch; batch.reserve(queries);
        // Check every baby and its opposite sign. Infinity is its own negative;
        // finite negative babies cannot belong to the small uint64 j domain.
        for (unsigned sign=0;sign<2;++sign) {
            for (uint64_t cursor=0;cursor<table.entries().size();) {
                batch.clear();
                const auto end=std::min<uint64_t>(cursor+queries,table.entries().size());
                for (auto i=cursor;i<end;++i) {
                    auto key=table.entries()[i].key;
                    if (sign && key.bytes[0]) key.bytes[0]^=1;
                    batch.push_back(key);
                }
                const auto result=prepared.probe(batch);
                for (size_t i=0;i<batch.size();++i) {
                    const auto expected=table.entries()[cursor+i].j;
                    const auto& hit=result.hits[i];
                    if ((!sign || expected==0) ? (hit.end-hit.begin!=1 || hit.j!=expected) : (hit.end!=hit.begin))
                        throw std::runtime_error("HIP BSGS baby/sign verification failed");
                }
                checked+=result.device_queries; kernel_ms+=result.kernel_ms;
                upload_ms+=result.upload_ms; download_ms+=result.download_ms;
                cursor=end;
            }
        }
    }
#endif
    const auto& p=table.memory();
    std::string checksum;
    for (uint8_t byte : table.checksum()) { checksum+="0123456789abcdef"[byte>>4]; checksum+="0123456789abcdef"[byte&15]; }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"{\"backend\":\""<<(action=="validate" ? "hip" : "cpu")<<"\",\"device\":"
        <<(action=="validate" ? std::to_string(device) : "null")<<",\"uuid\":\""<<device_uuid<<"\",\"format_version\":1,\"curve\":\"secp256k1\",\"mapping\":\"jG:0<=j<m\",\"m\":"<<p.m
        <<",\"buckets\":"<<p.buckets<<",\"bloom_words\":"<<p.bloom_words<<",\"bits_per_entry\":"<<table.bits_per_entry()
        <<",\"resident_bytes\":"<<p.resident_bytes<<",\"host_peak_bytes\":"<<p.host_peak_bytes<<",\"file_bytes\":"<<p.file_bytes
        <<",\"device_queries\":"<<checked<<",\"device_allocation_bytes\":"<<device_bytes
        <<",\"pinned_allocation_bytes\":"<<pinned_bytes<<",\"prepare_upload_ms\":"<<preparation_ms
        <<",\"query_kernel_ms\":"<<kernel_ms<<",\"query_upload_ms\":"<<upload_ms<<",\"query_download_ms\":"<<download_ms
        <<",\"checksum\":\""<<checksum<<"\",\"search_coverage\":false,\"wall_ms\":"<<elapsed<<"}\n";
    if (!std::cout) throw std::runtime_error("failed to write BSGS table report");
    return 0;
}
} // namespace keyhunt::backend
