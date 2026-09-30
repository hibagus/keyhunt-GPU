#include "keyhunt/storage/journal.h"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace keyhunt::backend {
namespace {
using namespace storage;
using Options=std::map<std::string,std::string>;
std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\')out << '\\' << c;
        else if(c<32)out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
std::string hex(const uint8_t* data,size_t size) {
    static constexpr char digits[]="0123456789abcdef";
    std::string out; out.reserve(size*2);
    for(size_t i=0;i<size;++i){out+=digits[data[i]>>4];out+=digits[data[i]&15];}
    return out;
}
std::vector<uint8_t> unhex(const std::string& value,size_t size) {
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
Digest digest(const std::string& value) {
    const auto bytes=unhex(value,32);Digest result{};
    std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
int64_t number(const std::string& value,int64_t maximum=INT64_MAX) {
    int64_t out=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),out);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || out<=0 || out>maximum)
        throw std::invalid_argument("expected a positive decimal integer within the allowed bound");
    return out;
}
std::string required(const Options& options,const std::string& name) {
    const auto it=options.find(name);
    if(it==options.end())throw std::invalid_argument("missing --"+name);
    return it->second;
}
std::string optional(const Options& options,const std::string& name,const std::string& fallback={}) {
    const auto it=options.find(name);return it==options.end()?fallback:it->second;
}
Scope scope(const Options& options) {return {required(options,"project"),digest(required(options,"job"))};}
int64_t lifetime(const Options& options) {return number(optional(options,"lifetime","2592000"),2592000);}

// Tokens carry the complete fence for local operators. They are plain data, not
// credentials; authenticated remote ownership belongs to the coordinator.
std::string token(const Grant& g) {
    return "v1:"+g.scope.project+":"+hex(g.scope.job.data(),32)+":"+g.owner+":"+
        hex(g.epoch.data(),g.epoch.size())+":"+std::to_string(g.generation)+":"+
        std::to_string(g.expires)+":"+g.block.hex();
}
Grant parse_grant(Journal& journal,const std::string& value) {
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
void print_grant(const Grant& g) {
    std::cout << "{\"project\":" << quote(g.scope.project) << ",\"job\":" << quote(hex(g.scope.job.data(),32))
        << ",\"block\":" << quote(g.block.hex()) << ",\"begin\":" << quote(g.interval.begin().hex())
        << ",\"end_exclusive\":" << quote(g.interval.end().hex()) << ",\"owner\":" << quote(g.owner)
        << ",\"generation\":" << quote(std::to_string(g.generation)) << ",\"expires_unix\":" << g.expires
        << ",\"grant\":" << quote(token(g)) << '}';
}
void print_intervals(const std::vector<ScalarInterval>& intervals) {
    std::cout << '[';
    for(size_t i=0;i<intervals.size();++i)
        std::cout << (i?",":"") << "{\"begin\":" << quote(intervals[i].begin().hex())
            << ",\"end_exclusive\":" << quote(intervals[i].end().hex()) << '}';
    std::cout << ']';
}
} // namespace

int state_command(int argc,char** argv) {
    // Action-specific option sets reject misspellings and irrelevant flags before
    // opening any database. In particular, there is no CLI for crediting coverage.
    static const std::map<std::string,std::set<std::string>> allowed{
        {"init",{}},{"project-create",{"name"}},
        {"job-create",{"project","mode","range","block-width","target-digest","algorithm-digest","seed"}},
        {"claim",{"project","job","owner","request","policy","count","block","window","lifetime"}},
        {"inspect",{"project","job"}},{"block",{"project","job","block"}},
        {"renew",{"grant","request","lifetime"}},{"return",{"grant","request"}},
        {"recover",{"project","job","block","owner","request","previous-stopped","lifetime"}},
        {"check",{}},{"compact",{}},{"backup",{"destination"}},{"restore",{"source"}}};
    if(argc<3)throw std::invalid_argument("usage: keyhunt state ACTION [--state-dir ABSOLUTE] [action options]; see docs/STORAGE.md");
    const std::string action=argv[2];const auto spec=allowed.find(action);
    if(spec==allowed.end())throw std::invalid_argument("unknown state action: "+action);
    Options options;
    for(int i=3;i<argc;i+=2) {
        const std::string flag=argv[i];
        if(i+1>=argc || flag.rfind("--",0)!=0)throw std::invalid_argument("state options require --name VALUE pairs");
        const auto key=flag.substr(2);
        if(key!="state-dir" && !spec->second.count(key))throw std::invalid_argument("unsupported option: "+flag);
        if(!options.emplace(key,argv[i+1]).second)throw std::invalid_argument("duplicate option: "+flag);
    }
    if(action=="restore") {
        Journal::restore(required(options,"source"),required(options,"state-dir"));
        std::cout << "{\"restored\":true,\"quarantined\":true}\n";
    } else {
        Journal journal(optional(options,"state-dir"));
        if(action=="init")std::cout << "{\"schema_version\":1,\"state_directory\":" << quote(journal.state_directory()) << '}';
        else if(action=="project-create")std::cout << "{\"project\":" << quote(journal.create_project(required(options,"name"))) << '}';
        else if(action=="job-create") {
            const auto mode=required(options,"mode");
            if(mode!="xpoint" && mode!="bsgs")throw std::invalid_argument("mode must be xpoint or bsgs");
            const auto range=required(options,"range");const auto colon=range.find(':');
            if(colon==std::string::npos || range.find(':',colon+1)!=std::string::npos)
                throw std::invalid_argument("range must be half-open HEX:HEX");
            const Manifest manifest{mode=="xpoint"?Mode::XPoint:Mode::Bsgs,
                ScalarInterval(UInt256::from_hex(range.substr(0,colon)),UInt256::from_hex(range.substr(colon+1))),
                UInt256::from_hex(required(options,"block-width")),digest(required(options,"target-digest")),
                digest(required(options,"algorithm-digest"))};
            std::optional<Digest> seed;if(options.count("seed"))seed=digest(required(options,"seed"));
            const auto id=journal.create_job(required(options,"project"),manifest,seed);
            std::cout << "{\"project\":" << quote(id.project) << ",\"job\":" << quote(hex(id.job.data(),32)) << '}';
        } else if(action=="claim") {
            Selection selection;const auto policy=optional(options,"policy","sequential");
            if(policy=="sequential")selection.policy=Policy::Sequential;
            else if(policy=="random")selection.policy=Policy::Random;
            else if(policy=="random-window")selection.policy=Policy::RandomWindow;
            else if(policy=="manual")selection.policy=Policy::Manual;
            else throw std::invalid_argument("unknown allocation policy");
            selection.count=uint32_t(number(optional(options,"count","1"),256));
            if(policy=="manual")selection.block=UInt256::from_hex(required(options,"block"));
            else if(options.count("block"))throw std::invalid_argument("--block requires manual policy");
            if(options.count("window")) {
                if(policy!="random-window")throw std::invalid_argument("--window requires random-window policy");
                selection.window=UInt256::from_hex(required(options,"window"));
            }
            const auto grants=journal.claim(scope(options),required(options,"owner"),required(options,"request"),selection,lifetime(options));
            std::cout << "{\"assignments\":[";
            for(size_t i=0;i<grants.size();++i){if(i)std::cout << ',';print_grant(grants[i]);}
            std::cout << "]}";
        } else if(action=="inspect") {
            const auto s=journal.statistics(scope(options));
            std::cout << "{\"blocks\":" << quote(s.blocks.hex()) << ",\"unexplored\":" << quote(s.unexplored.hex())
                << ",\"finished\":" << quote(s.finished.hex()) << ",\"assignments\":" << s.assignments
                << ",\"coverage_intervals\":" << s.coverage_intervals << ",\"finished_runs\":" << s.finished_runs
                << ",\"tree_nodes\":" << s.tree_nodes << ",\"requests\":" << s.requests << ",\"events\":" << s.events
                << ",\"database_bytes\":" << s.database_bytes << ",\"wal_bytes\":" << s.wal_bytes
                << ",\"free_pages\":" << s.free_pages << ",\"quarantined\":" << (s.quarantined?"true":"false") << '}';
        } else if(action=="block") {
            const auto b=journal.block(scope(options),UInt256::from_hex(required(options,"block")));
            std::cout << "{\"state\":" << quote(b.state) << ",\"started\":" << (b.started?"true":"false")
                << ",\"expired\":" << (b.expired?"true":"false") << ",\"assignment\":";
            if(b.assignment)print_grant(*b.assignment);else std::cout << "null";
            std::cout << ",\"covered\":";print_intervals(b.covered);
            std::cout << ",\"remaining\":";print_intervals(b.remaining);std::cout << '}';
        } else if(action=="renew") {
            print_grant(journal.renew(parse_grant(journal,required(options,"grant")),required(options,"request"),lifetime(options)));
        } else if(action=="return") {
            journal.return_unstarted(parse_grant(journal,required(options,"grant")),required(options,"request"));
            std::cout << "{\"returned\":true}";
        } else if(action=="recover") {
            const auto stopped=optional(options,"previous-stopped","no");
            if(stopped!="yes" && stopped!="no")throw std::invalid_argument("--previous-stopped must be yes or no");
            print_grant(journal.recover(scope(options),UInt256::from_hex(required(options,"block")),
                required(options,"owner"),required(options,"request"),stopped=="yes",lifetime(options)));
        } else if(action=="check"){journal.check();std::cout << "{\"integrity\":\"ok\"}";}
        else if(action=="compact"){journal.compact();std::cout << "{\"compacted\":true}";}
        else if(action=="backup"){journal.backup(required(options,"destination"));std::cout << "{\"backed_up\":true,\"quarantined\":true}";}
        std::cout << '\n';
    }
    // A broken output stream does not undo a committed claim. Retry the same
    // request ID to obtain its durable receipt, including the original fence.
    std::cout.flush();
    if(!std::cout)throw std::runtime_error("state output failed; retry mutations with the same request ID");
    return 0;
}
} // namespace keyhunt::backend
