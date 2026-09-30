#include "keyhunt/core/bsgs_table.h"
#include "keyhunt/core/xpoint_search.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace keyhunt::bsgs {
namespace {
constexpr uint64_t header_size=128, digest_size=32;
uint64_t add(uint64_t a,uint64_t b) {
    if (b>UINT64_MAX-a) throw std::overflow_error("BSGS size addition overflow");
    return a+b;
}
uint64_t multiply(uint64_t a,uint64_t b) {
    if (b && a>UINT64_MAX/b) throw std::overflow_error("BSGS size multiplication overflow");
    return a*b;
}
uint64_t power_two(uint64_t value) {
    uint64_t result=1;
    while (result<value) { if (result>(UINT64_MAX>>1)) throw std::overflow_error("BSGS geometry overflow"); result*=2; }
    return result;
}
void put(uint8_t* p,uint64_t value,unsigned width) { for (unsigned i=0;i<width;++i) p[i]=uint8_t(value>>(8*i)); }
uint64_t get(const uint8_t* p,unsigned width) {
    uint64_t value=0; for (unsigned i=0;i<width;++i) value |= uint64_t(p[i])<<(8*i); return value;
}
std::array<uint8_t,32> digest(const uint8_t* bytes,size_t count) {
    alignas(uint64_t) std::array<uint8_t,32> value{};
    sha256(const_cast<uint8_t*>(bytes),count,value.data());
    return value;
}
Key derive(const core::XPointVerifier& verifier,uint64_t j) {
    Key key{};
    if (!j) return key; // zero baby step is the point at infinity, not private key 0
    const auto pub=verifier.derive(core::UInt256(j));
    key.bytes[0]=uint8_t(2+(pub[64]&1));
    std::copy_n(pub.begin()+1,32,key.bytes+1);
    return key;
}
void io_error(const char* operation) { throw std::runtime_error(std::string(operation)+": "+std::strerror(errno)); }
}
MemoryPlan plan(uint64_t m,Options options) {
    if (!m) throw std::invalid_argument("BSGS m must be positive");
    if (options.bits_per_entry!=8 && options.bits_per_entry!=16 && options.bits_per_entry!=32)
        throw std::invalid_argument("BSGS bits per entry must be 8, 16 or 32");
    MemoryPlan p;
    p.m=m; p.buckets=power_two(add(m,7)/8);
    p.bloom_words=power_two(std::max<uint64_t>(64,multiply(m,options.bits_per_entry)))/64;
    p.resident_bytes=add(add(multiply(m,48),multiply(add(p.buckets,1),8)),multiply(p.bloom_words,8));
    p.file_bytes=add(add(header_size,p.resident_bytes),digest_size);
    // Includes resident arrays, one canonical serialized buffer, index-validation
    // bitmap, CPU generator context and a 64 KiB allowance for stack/I/O state.
    p.host_peak_bytes=add(add(p.resident_bytes,p.file_bytes),add(add(m,7)/8,add(sizeof(Secp256K1),65536)));
    if (p.host_peak_bytes>options.host_memory_bytes || p.file_bytes>std::numeric_limits<size_t>::max() ||
        p.file_bytes>uint64_t(std::numeric_limits<std::streamsize>::max()))
        throw std::invalid_argument("BSGS table exceeds the host memory budget");
    return p;
}
void require_device_memory(uint64_t needed,uint64_t free,uint64_t reserve) {
    if (reserve>free || needed>free-reserve) throw std::runtime_error("insufficient HIP memory after reserved headroom");
}
Table Table::build(uint64_t m,Options options) {
    Table table; table.memory_=plan(m,options); table.bits_=options.bits_per_entry;
    table.entries_.resize(m); table.offsets_.resize(table.memory_.buckets+1); table.bloom_.resize(table.memory_.bloom_words);
    core::XPointVerifier verifier;
    for (uint64_t j=0;j<m;++j) table.entries_[j]={derive(verifier,j),{},j};
    const auto mask=table.memory_.buckets-1;
    // Contiguous bucket spans avoid pointer chasing and open-addressing probe caps.
    std::sort(table.entries_.begin(),table.entries_.end(),[mask](const Entry& a,const Entry& b) {
        const auto ba=hash(a.key)&mask,bb=hash(b.key)&mask;
        if (ba!=bb) return ba<bb;
        const auto c=compare(a.key,b.key); return c ? c<0 : a.j<b.j;
    });
    for (const auto& entry : table.entries_) {
        const auto value=hash(entry.key);
        ++table.offsets_[(value&mask)+1];
        for (unsigned probe=0;probe<7;++probe) {
            const auto bit=bloom_bit(value,probe,table.memory_.bloom_words);
            table.bloom_[bit>>6] |= 1ULL<<(bit&63);
        }
    }
    for (uint64_t b=1;b<table.offsets_.size();++b) table.offsets_[b]+=table.offsets_[b-1];
    const auto bytes=table.encode();
    std::copy_n(bytes.end()-digest_size,digest_size,table.checksum_.begin());
    return table;
}
std::vector<uint8_t> Table::encode() const {
    std::vector<uint8_t> bytes(memory_.file_bytes,0);
    std::copy_n(reinterpret_cast<const uint8_t*>("KHBSGS1\0"),8,bytes.begin());
    // Explicit field offsets, never native C++ struct serialization.
    for (unsigned offset : {8U,16U,20U,24U,28U,32U}) put(bytes.data()+offset,1,4);
    put(bytes.data()+12,header_size,4); put(bytes.data()+36,7,4);
    put(bytes.data()+40,memory_.m,8); put(bytes.data()+48,memory_.buckets,8);
    put(bytes.data()+56,memory_.bloom_words,8); put(bytes.data()+64,bits_,4); put(bytes.data()+68,48,4);
    put(bytes.data()+72,offsets_.size()*8,8); put(bytes.data()+80,entries_.size()*48,8);
    put(bytes.data()+88,bloom_.size()*8,8); put(bytes.data()+96,memory_.resident_bytes,8);
    size_t pos=header_size;
    for (auto offset : offsets_) { put(bytes.data()+pos,offset,8); pos+=8; }
    for (const auto& entry : entries_) {
        std::copy_n(entry.key.bytes,33,bytes.data()+pos);
        put(bytes.data()+pos+40,entry.j,8); pos+=48;
    }
    for (auto word : bloom_) { put(bytes.data()+pos,word,8); pos+=8; }
    const auto checksum=digest(bytes.data(),pos);
    std::copy(checksum.begin(),checksum.end(),bytes.begin()+pos);
    return bytes;
}
void Table::validate() const {
    if (offsets_.front()!=0 || offsets_.back()!=memory_.m) throw std::runtime_error("BSGS invalid bucket endpoints");
    std::vector<uint8_t> seen((memory_.m+7)/8,0);
    core::XPointVerifier verifier;
    for (uint64_t b=0;b<memory_.buckets;++b) {
        if (offsets_[b]>offsets_[b+1] || offsets_[b+1]>memory_.m) throw std::runtime_error("BSGS invalid bucket span");
        for (auto i=offsets_[b];i<offsets_[b+1];++i) {
            const auto& entry=entries_[i];
            if ((hash(entry.key)&(memory_.buckets-1))!=b || entry.j>=memory_.m)
                throw std::runtime_error("BSGS invalid bucket or baby index");
            if (i>offsets_[b] && (compare(entries_[i-1].key,entry.key)>0 ||
                (compare(entries_[i-1].key,entry.key)==0 && entries_[i-1].j>=entry.j)))
                throw std::runtime_error("BSGS unsorted collision list");
            if (seen[entry.j>>3]&(1U<<(entry.j&7))) throw std::runtime_error("BSGS duplicate baby index");
            seen[entry.j>>3] |= uint8_t(1U<<(entry.j&7));
            if (compare(entry.key,derive(verifier,entry.j))) throw std::runtime_error("BSGS baby point does not equal jG");
            if (!maybe_contains(view(),entry.key)) throw std::runtime_error("BSGS Bloom filter has a false negative");
        }
    }
}
Table Table::load(const std::string& path,Options options) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("cannot open BSGS table: "+path);
    std::array<uint8_t,header_size> header{};
    input.read(reinterpret_cast<char*>(header.data()),header.size());
    if (!input || std::memcmp(header.data(),"KHBSGS1\0",8))
        throw std::runtime_error("not a versioned BSGS table; rebuild legacy .blm/.tbl caches explicitly");
    for (unsigned offset : {8U,16U,20U,24U,28U,32U})
        if (get(header.data()+offset,4)!=1) throw std::runtime_error("unsupported BSGS format/curve/mapping/hash version");
    if (get(header.data()+12,4)!=header_size || get(header.data()+36,4)!=7 || get(header.data()+68,4)!=48)
        throw std::runtime_error("unsupported BSGS header/entry/filter format");
    for (unsigned i=104;i<header_size;++i) if (header[i]) throw std::runtime_error("BSGS reserved header bytes are nonzero");
    options.bits_per_entry=uint32_t(get(header.data()+64,4));
    Table table; table.memory_=plan(get(header.data()+40,8),options); table.bits_=options.bits_per_entry;
    const auto& p=table.memory_;
    if (get(header.data()+48,8)!=p.buckets || get(header.data()+56,8)!=p.bloom_words ||
        get(header.data()+72,8)!=(p.buckets+1)*8 || get(header.data()+80,8)!=p.m*48 ||
        get(header.data()+88,8)!=p.bloom_words*8 || get(header.data()+96,8)!=p.resident_bytes)
        throw std::runtime_error("BSGS inconsistent section sizes");
    input.seekg(0,std::ios::end);
    if (input.tellg()!=std::streamoff(p.file_bytes)) throw std::runtime_error("BSGS truncated table or trailing bytes");
    std::vector<uint8_t> bytes(p.file_bytes);
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if (!input || !std::equal(header.begin(),header.end(),bytes.begin())) throw std::runtime_error("BSGS table changed while loading");
    table.checksum_=digest(bytes.data(),bytes.size()-digest_size);
    if (!std::equal(table.checksum_.begin(),table.checksum_.end(),bytes.end()-digest_size))
        throw std::runtime_error("BSGS checksum mismatch");
    table.offsets_.resize(p.buckets+1); table.entries_.resize(p.m); table.bloom_.resize(p.bloom_words);
    size_t pos=header_size;
    for (auto& offset : table.offsets_) { offset=get(bytes.data()+pos,8); pos+=8; }
    for (auto& entry : table.entries_) {
        std::copy_n(bytes.data()+pos,33,entry.key.bytes);
        for (unsigned i=33;i<40;++i) if (bytes[pos+i]) throw std::runtime_error("BSGS reserved entry bytes are nonzero");
        entry.j=get(bytes.data()+pos+40,8); pos+=48;
    }
    for (auto& word : table.bloom_) { word=get(bytes.data()+pos,8); pos+=8; }
    // A checksum detects corruption, not semantic correctness. Validate every
    // unique j and jG relation, so even a rehashed wrong cache cannot hide work.
    table.validate();
    return table;
}
void Table::save(const std::string& path) const {
    const auto bytes=encode();
    // Publish a complete, synced file with link(2)'s exclusive destination
    // semantics. The temporary file is on the same filesystem as its destination.
    std::string name=path+".tmp.XXXXXX";
    std::vector<char> temporary(name.begin(),name.end()); temporary.push_back('\0');
    const int fd=mkstemp(temporary.data());
    if (fd<0) io_error("create BSGS temporary file");
    bool open=true;
    try {
        size_t offset=0;
        while (offset<bytes.size()) {
            const auto written=write(fd,bytes.data()+offset,std::min<size_t>(bytes.size()-offset,1048576));
            if (written<0 && errno==EINTR) continue;
            if (written<=0) io_error("write BSGS table");
            offset+=size_t(written);
        }
        if (fsync(fd)) io_error("sync BSGS table");
        const int closed=close(fd); open=false;
        if (closed) io_error("close BSGS table");
        if (link(temporary.data(),path.c_str())) io_error("publish BSGS table (destination must not exist)");
        if (unlink(temporary.data())) io_error("remove BSGS temporary file");
        const auto parent=std::filesystem::path(path).parent_path();
        const int directory=::open(parent.empty()?".":parent.c_str(),O_RDONLY|O_DIRECTORY);
        if (directory<0) io_error("open BSGS parent directory");
        const int synced=fsync(directory); const int saved_errno=errno; close(directory); errno=saved_errno;
        if (synced) io_error("sync BSGS parent directory (published file remains)");
    } catch (...) { if (open) close(fd); unlink(temporary.data()); throw; }
}
} // namespace keyhunt::bsgs
