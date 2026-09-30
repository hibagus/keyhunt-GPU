#pragma once
#include <cstdint>
#include <cstddef>
#if defined(__HIPCC__) || defined(__CUDACC__)
#define KEYHUNT_TABLE_HD __host__ __device__
#else
#define KEYHUNT_TABLE_HD
#endif

namespace keyhunt::bsgs {
// SEC1 compressed key (02/03 || big-endian X); 33 zero bytes uniquely encode
// infinity. Including parity prevents P and -P from being confused by lookup.
struct Key { uint8_t bytes[33]{}; };
struct Entry { Key key; uint8_t reserved[7]{}; uint64_t j = 0; };
static_assert(sizeof(Entry)==48 && offsetof(Entry,j)==40,"unexpected device entry layout");
struct MatchRange { uint64_t begin = 0, end = 0; };
struct View {
    const Entry* entries = nullptr;
    const uint64_t* offsets = nullptr;
    const uint64_t* bloom = nullptr;
    uint64_t count = 0, buckets = 0, bloom_words = 0;
};
KEYHUNT_TABLE_HD inline int compare(const Key& a,const Key& b) {
    for (unsigned i=0;i<33;++i) if (a.bytes[i]!=b.bytes[i]) return a.bytes[i]<b.bytes[i] ? -1 : 1;
    return 0;
}
// Version 1 hash: FNV-1a over all 33 bytes, then this specified 64-bit avalanche.
// Unsigned wrap is intentional and identical on CPU, HIP and future CUDA.
KEYHUNT_TABLE_HD inline uint64_t mix(uint64_t x) {
    x ^= x>>30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x>>27; x *= 0x94d049bb133111ebULL;
    return x^(x>>31);
}
KEYHUNT_TABLE_HD inline uint64_t hash(const Key& key) {
    uint64_t value=0xcbf29ce484222325ULL;
    for (uint8_t byte : key.bytes) { value ^= byte; value *= 0x100000001b3ULL; }
    return mix(value);
}
KEYHUNT_TABLE_HD inline uint64_t bloom_bit(uint64_t hash_value,unsigned probe,uint64_t words) {
    const uint64_t stride=mix(hash_value^0x9e3779b97f4a7c15ULL)|1ULL;
    return (hash_value+uint64_t(probe)*stride)&(words*64-1);
}
KEYHUNT_TABLE_HD inline bool maybe_contains(const View& table,const Key& key) {
    const uint64_t value=hash(key);
    for (unsigned probe=0;probe<7;++probe) {
        const auto bit=bloom_bit(value,probe,table.bloom_words);
        if (!(table.bloom[bit>>6]&(1ULL<<(bit&63)))) return false;
    }
    return true;
}
KEYHUNT_TABLE_HD inline MatchRange lookup(const View& table,const Key& key,bool use_filter=true) {
    if (use_filter && !maybe_contains(table,key)) return {};
    const auto bucket=hash(key)&(table.buckets-1);
    const uint64_t end=table.offsets[bucket+1];
    uint64_t lo=table.offsets[bucket], hi=end;
    while (lo<hi) {
        const auto mid=lo+(hi-lo)/2;
        if (compare(table.entries[mid].key,key)<0) lo=mid+1; else hi=mid;
    }
    const auto first=lo;
    // Return the entire equal-key list, not an arbitrary bucket representative.
    // Valid v1 jG tables have unique points; synthetic/future tables can collide.
    hi=end;
    while (lo<hi) {
        const auto mid=lo+(hi-lo)/2;
        if (compare(table.entries[mid].key,key)<=0) lo=mid+1; else hi=mid;
    }
    return first==lo ? MatchRange{} : MatchRange{first,lo};
}
} // namespace keyhunt::bsgs
#undef KEYHUNT_TABLE_HD
