#pragma once

#include "keyhunt/core/config.h"
#include "keyhunt/crypto/secp256k1/Point.h"
#include "bloom/bloom.h"
#include <cstdint>
#include <memory>
#include <vector>

class Secp256K1;

namespace keyhunt::core {

// Native CPU cache entry. X-point mode stores only its historical 20-byte prefix.
// Do not use this type as an exact X target or portable device/file record.
struct address_value { uint8_t value[20]; };

// One table per CPU search. Load once, sort, then treat entries/filter as read-only.
// Retains established text parsing and native cache behavior for the CLI adapter.
class CpuTargetTable {
public:
    explicit CpuTargetTable(const SearchConfig& config) : config(config) {}
    ~CpuTargetTable();
    CpuTargetTable(const CpuTargetTable&) = delete;
    CpuTargetTable& operator=(const CpuTargetTable&) = delete;

    bool readFileAddress(const char* fileName);
    void writeFileIfNeeded(const char* fileName);

    struct bloom filter{};
    address_value* entries = nullptr;
    uint64_t count = 0;
    int match_bytes = -1;
    int cache_loaded = 0;

private:
    const SearchConfig& config;
    bool forceReadFileAddress(const char* fileName);
    bool forceReadFileAddressEth(const char* fileName);
    bool forceReadFileXPoint(const char* fileName);
};

struct BsgsTargets {
    std::vector<Point> points;
    std::unique_ptr<bool[]> compressed;
};
BsgsTargets loadBsgsTargets(const char* fileName, Secp256K1& curve);
bool readVanityTargets(const char* fileName, int existing_targets, int (*add_target)(char*));
bool isBase58(char c);
bool isValidBase58String(char* text);
bool initBloomFilter(struct bloom* filter, uint64_t items, int multiplier);
void checkpointer(void* ptr, const char* file, const char* function, const char* name, int line);

} // namespace keyhunt::core
