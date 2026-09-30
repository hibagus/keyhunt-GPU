#include "keyhunt/core/bsgs_table.h"
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
    const char* usage="usage: keyhunt bsgs-table build --m N --output FILE [--bits-per-entry 8|16|32] [--host-memory BYTES] OR bsgs-table inspect --input FILE [--host-memory BYTES]";
    if (argc<3) throw std::invalid_argument(usage);
    const std::string action=argv[2];
    if (action!="build" && action!="inspect") throw std::invalid_argument(usage);
    std::map<std::string,std::string> args;
    for (int i=3;i<argc;i+=2) {
        if (i+1>=argc) throw std::invalid_argument(usage);
        const std::string key=argv[i];
        if (key!="--host-memory" && !(action=="build" && (key=="--m" || key=="--output" || key=="--bits-per-entry")) &&
            !(action=="inspect" && key=="--input")) throw std::invalid_argument(usage);
        if (!args.emplace(key,argv[i+1]).second) throw std::invalid_argument("duplicate BSGS option");
    }
    bsgs::Options options;
    if (args.count("--host-memory")) options.host_memory_bytes=number(args["--host-memory"]);
    if (args.count("--bits-per-entry")) {
        const auto value=number(args["--bits-per-entry"]);
        if (value>32) throw std::invalid_argument(usage);
        options.bits_per_entry=uint32_t(value);
    }
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
    const auto& p=table.memory();
    std::string checksum;
    for (uint8_t byte : table.checksum()) { checksum+="0123456789abcdef"[byte>>4]; checksum+="0123456789abcdef"[byte&15]; }
    const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"{\"format_version\":1,\"curve\":\"secp256k1\",\"mapping\":\"jG:0<=j<m\",\"m\":"<<p.m
        <<",\"buckets\":"<<p.buckets<<",\"bloom_words\":"<<p.bloom_words<<",\"bits_per_entry\":"<<table.bits_per_entry()
        <<",\"resident_bytes\":"<<p.resident_bytes<<",\"host_peak_bytes\":"<<p.host_peak_bytes<<",\"file_bytes\":"<<p.file_bytes
        <<",\"checksum\":\""<<checksum<<"\",\"search_coverage\":false,\"wall_ms\":"<<elapsed<<"}\n";
    if (!std::cout) throw std::runtime_error("failed to write BSGS table report");
    return 0;
}
} // namespace keyhunt::backend
