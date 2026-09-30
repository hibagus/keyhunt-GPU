#pragma once
#include "keyhunt/storage/journal.h"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace keyhunt::backend::state_detail {
using namespace storage;
using Options=std::map<std::string,std::string>;
inline std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\')out << '\\' << c;
        else if(c<32)out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
inline std::string hex(const uint8_t* data,size_t size) {
    static constexpr char digits[]="0123456789abcdef";
    std::string out; out.reserve(size*2);
    for(size_t i=0;i<size;++i){out+=digits[data[i]>>4];out+=digits[data[i]&15];}
    return out;
}
inline std::vector<uint8_t> unhex(const std::string& value,size_t size) {
    if(value.size()!=size*2)throw std::invalid_argument("incorrect fixed hexadecimal length");
    auto digit=[](char c)->unsigned {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        throw std::invalid_argument("invalid hexadecimal digit");
    };
    std::vector<uint8_t> out(size);
    for(size_t i=0;i<size;++i)out[i]=uint8_t((digit(value[2*i])<<4)|digit(value[2*i+1]));
    return out;
}
inline Digest digest(const std::string& value) {
    const auto bytes=unhex(value,32);Digest result{};
    std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
inline int64_t number(const std::string& value,int64_t maximum=INT64_MAX) {
    int64_t out=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),out);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || out<=0 || out>maximum)
        throw std::invalid_argument("expected a positive decimal integer within the allowed bound");
    return out;
}
inline std::string required(const Options& options,const std::string& name) {
    const auto it=options.find(name);
    if(it==options.end())throw std::invalid_argument("missing --"+name);
    return it->second;
}
inline std::string optional(const Options& options,const std::string& name,const std::string& fallback={}) {
    const auto it=options.find(name);return it==options.end()?fallback:it->second;
}
inline Scope scope(const Options& options) {return {required(options,"project"),digest(required(options,"job"))};}
inline int64_t lifetime(const Options& options) {return number(optional(options,"lifetime","2592000"),2592000);}

// Tokens carry the complete fence for local operators. They are plain data, not
// credentials; authenticated remote ownership belongs to the coordinator.
inline std::string token(const Grant& g) {
    return "v1:"+g.scope.project+":"+hex(g.scope.job.data(),32)+":"+g.owner+":"+
        hex(g.epoch.data(),g.epoch.size())+":"+std::to_string(g.generation)+":"+
        std::to_string(g.expires)+":"+g.block.hex();
}
inline Grant parse_grant(Journal& journal,const std::string& value) {
    if(value.size()>512)throw std::invalid_argument("grant token too long");
    std::vector<std::string> fields;size_t begin=0;
    while(true){
        const auto end=value.find(':',begin);fields.push_back(value.substr(begin,end-begin));
        if(end==std::string::npos)break;
        begin=end+1;
    }
    if(fields.size()!=8 || fields[0]!="v1")throw std::invalid_argument("invalid v1 grant token");
    const Scope id{fields[1],digest(fields[2])};
    const auto manifest=journal.manifest(id);const auto block=UInt256::from_hex(fields[7]);
    return {id,block,scheduler::BlockGrid(manifest.root,manifest.block_width).block(block),fields[3],
        number(fields[5]),number(fields[6]),unhex(fields[4],16)};
}
} // namespace keyhunt::backend::state_detail
