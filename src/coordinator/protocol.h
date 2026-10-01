#pragma once
#include "keyhunt/coordinator/repository.h"
#include "checkpoint_data.h"
#include <set>

namespace keyhunt::coordination::wire {
using namespace storage;
using storage::detail::Bytes;
inline void fields(const Json& j,std::initializer_list<const char*> required,
                   std::initializer_list<const char*> optional={}){
    if(!j.is_object())throw Error(400,"expected an object");
    std::set<std::string> keys;
    for(const auto* key:required){keys.insert(key);if(!j.contains(key))throw Error(400,std::string("missing field: ")+key);}
    for(const auto* key:optional)keys.insert(key);
    for(auto it=j.begin();it!=j.end();++it)if(!keys.count(it.key()))throw Error(400,"unknown field: "+it.key());
}
inline std::string str(const Json& j,const char* key,size_t maximum=256){
    if(!j.contains(key)||!j[key].is_string())throw Error(400,std::string("expected string: ")+key);
    auto s=j[key].get<std::string>();
    if(s.empty()||s.size()>maximum||s.find('\0')!=std::string::npos)throw Error(400,"invalid string length");
    return s;
}
inline int64_t integer(const Json& j,const char* key,int64_t minimum=0,int64_t maximum=INT64_MAX){
    if(!j.contains(key)||!j[key].is_number_integer())throw Error(400,std::string("expected integer: ")+key);
    if(j[key].is_number_unsigned()&&j[key].get<uint64_t>()>uint64_t(INT64_MAX))throw Error(400,"integer overflow");
    const auto n=j[key].get<int64_t>();if(n<minimum||n>maximum)throw Error(400,"integer out of bounds");return n;
}
inline bool boolean(const Json& j,const char* key){
    if(!j.contains(key)||!j[key].is_boolean())throw Error(400,std::string("expected boolean: ")+key);
    return j[key].get<bool>();
}
inline void token(const std::string& s){
    if(s.empty()||s.size()>128)throw Error(400,"invalid token length");
    for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='@'))
        throw Error(400,"invalid token character");
}
inline std::string hex(const Bytes& b){
    const char* h="0123456789abcdef";std::string s;s.reserve(b.size()*2);
    for(auto v:b){s+=h[v>>4];s+=h[v&15];}return s;
}
inline Bytes unhex(const std::string& s,size_t maximum=2*1024*1024){
    if(s.size()%2 || s.size()/2>maximum)throw Error(400,"invalid hex width");
    auto digit=[](char c)->uint8_t{if(c>='0'&&c<='9')return uint8_t(c-'0');if(c>='a'&&c<='f')return uint8_t(c-'a'+10);throw Error(400,"hex must be lowercase canonical");};
    Bytes b;for(size_t i=0;i<s.size();i+=2)b.push_back(uint8_t(digit(s[i])*16+digit(s[i+1])));return b;
}
inline Digest digest(const std::string& s){auto b=unhex(s,32);if(b.size()!=32)throw Error(400,"digest must have 32 bytes");Digest d;std::copy(b.begin(),b.end(),d.begin());return d;}
inline Bytes bytes(const Digest& d){return Bytes(d.begin(),d.end());}
inline UInt256 wide(const std::string& s){const auto n=UInt256::from_hex(s);if(n.hex()!=s)throw Error(400,"wide integers require canonical 0x plus 64 hex digits");return n;}
inline void bind_scope(detail::Statement& q,const Scope& s){q.bind(1,s.project);q.bind(2,bytes(s.job));}
inline Scope scope(const Json& j){return {str(j,"project",36),digest(str(j,"job",64))};}
inline Json interval(const ScalarInterval& v){return {{"begin",v.begin().hex()},{"end_exclusive",v.end().hex()}};}
inline Json grant(const Grant& g){return {{"project",g.scope.project},{"job",hex(bytes(g.scope.job))},{"block",g.block.hex()},
    {"begin",g.interval.begin().hex()},{"end_exclusive",g.interval.end().hex()},{"owner",g.owner},
    {"generation",g.generation},{"expires",g.expires},{"epoch",hex(g.epoch)}};}
inline Grant grant(const Json& j){
    fields(j,{"project","job","block","begin","end_exclusive","owner","generation","expires","epoch"});
    auto epoch=unhex(str(j,"epoch",32),16);if(epoch.size()!=16)throw Error(400,"invalid coordinator epoch");
    auto owner=str(j,"owner",128);token(owner);
    return {scope(j),wide(str(j,"block",66)),ScalarInterval(wide(str(j,"begin",66)),wide(str(j,"end_exclusive",66))),
        owner,integer(j,"generation",1),integer(j,"expires",1),epoch};
}
inline Mode mode(const std::string& name){
    if(name=="xpoint")return Mode::XPoint;
    if(name=="bsgs")return Mode::Bsgs;
    if(name=="hash160")return Mode::Hash160;
    if(name=="ethereum")return Mode::Ethereum;
    if(name=="vanity")return Mode::Vanity;
    throw Error(400,"unknown search mode");
}
inline Json manifest(const Scope& s,const Manifest& m){
    return {{"project",s.project},{"job",hex(bytes(s.job))},{"mode",mode_name(m.mode)},
        {"begin",m.root.begin().hex()},{"end_exclusive",m.root.end().hex()},{"block_width",m.block_width.hex()},
        {"target_digest",hex(bytes(m.targets))},{"algorithm_digest",hex(bytes(m.algorithm))}};
}
} // namespace keyhunt::coordination::wire
