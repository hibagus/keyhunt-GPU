#pragma once
#include "keyhunt/core/bsgs_layout.h"
#include <array>
#include <string>
#include <vector>

namespace keyhunt::bsgs {
struct Options {
    uint32_t bits_per_entry = 16; // supported policies: 8, 16, 32; seven probes
    uint64_t host_memory_bytes = 1024ULL*1024*1024;
};
struct MemoryPlan {
    uint64_t m=0, buckets=0, bloom_words=0;
    uint64_t resident_bytes=0, file_bytes=0, host_peak_bytes=0;
};
// Checked geometry before allocation. The cap is a caller budget, never a
// promise inferred from physical-package HBM or the current partition count.
MemoryPlan plan(uint64_t m,Options options={});
void require_device_memory(uint64_t needed,uint64_t free,uint64_t reserve);

class Table {
public:
    Table(Table&&) = default;
    Table& operator=(Table&&) = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    static Table build(uint64_t m,Options options={});
    static Table load(const std::string& path,Options options={});
    // Exclusive atomic publication: an existing file is never replaced.
    void save(const std::string& path) const;
    const MemoryPlan& memory() const { return memory_; }
    uint32_t bits_per_entry() const { return bits_; }
    const std::array<uint8_t,32>& checksum() const { return checksum_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const std::vector<uint64_t>& offsets() const { return offsets_; }
    const std::vector<uint64_t>& bloom() const { return bloom_; }
    View view() const { return {entries_.data(),offsets_.data(),bloom_.data(),memory_.m,memory_.buckets,memory_.bloom_words}; }
private:
    Table() = default;
    MemoryPlan memory_;
    uint32_t bits_=16;
    std::vector<Entry> entries_;
    std::vector<uint64_t> offsets_,bloom_;
    std::array<uint8_t,32> checksum_{};
    std::vector<uint8_t> encode() const;
    void validate() const;
};
} // namespace keyhunt::bsgs
