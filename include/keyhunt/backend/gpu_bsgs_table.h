#pragma once
#include "keyhunt/core/bsgs_table.h"
#include <memory>

namespace keyhunt::backend {
struct BsgsUploadOptions {
    uint64_t memory_reserve_bytes=64*1024*1024;
    uint32_t max_queries=4096;
    uint64_t host_memory_bytes=1024ULL*1024*1024;
};
struct BsgsProbeHit {
    uint64_t begin=0,end=0,j=UINT64_MAX;
    uint32_t bloom_positive=0,reserved=0;
};
struct BsgsProbeResult {
    std::vector<BsgsProbeHit> hits;
    uint64_t device_queries=0;
    float upload_ms=0,kernel_ms=0,download_ms=0;
    double wall_ms=0;
};
// Immutable prepared upload, reusable across compatible jobs. The host Table
// must outlive this owner; no second resident host copy is made. Single host
// owner; probe() is a bounded synchronous validation API, not a search receipt.
class GpuBsgsTable {
public:
    GpuBsgsTable(int device,const bsgs::Table& table,BsgsUploadOptions options={});
    ~GpuBsgsTable();
    GpuBsgsTable(const GpuBsgsTable&)=delete;
    GpuBsgsTable& operator=(const GpuBsgsTable&)=delete;
    BsgsProbeResult probe(const std::vector<bsgs::Key>& keys,bool use_filter=true);
    // Borrow only on this device, keep the owner alive, and drain all external
    // consumer streams before destruction. C11's search executor owns that duty.
    bsgs::View device_view() const;
    uint64_t device_bytes() const;
    uint64_t pinned_bytes() const;
    float preparation_upload_ms() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
