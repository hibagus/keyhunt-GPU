#pragma once
#include "keyhunt/backend/hip_executor.h"
#include "keyhunt/core/bsgs_search.h"

namespace keyhunt::backend {
struct BsgsSearchOptions {
    uint64_t max_steps=1048576; // target giant steps, not scalar coverage
    uint32_t candidate_capacity=1024;
    unsigned group_size=0; // 0 selects by visible CU count; explicit 1 or 8 also supported
    uint64_t memory_reserve_bytes=64*1024*1024;
    uint64_t host_memory_bytes=1024ULL*1024*1024;
};
struct BsgsSearchResult {
    core::BsgsBatch batch;
    std::vector<core::BsgsMatch> matches;
    bool overflow=false;
    unsigned group_size=0; // actual dispatched kernel
    uint64_t verified_steps=0,device_steps=0,candidate_count=0,tail_rejections=0;
    uint64_t device_allocation_bytes=0,pinned_allocation_bytes=0,download_bytes=0;
    float kernel_ms=0,download_ms=0;
    double seed_ms=0,verification_ms=0,wall_ms=0;
};
// One bounded asynchronous result slot with immutable target/table uploads.
// The host table and verifier must outlive the executor. Single host owner;
// cleanup drains the consumer stream before releasing its prepared table.
class HipBsgsExecutor {
public:
    HipBsgsExecutor(int device,const bsgs::Table& table,core::BsgsTargets targets,
        const core::XPointVerifier& verifier,BsgsSearchOptions options={});
    ~HipBsgsExecutor();
    HipBsgsExecutor(const HipBsgsExecutor&)=delete;
    HipBsgsExecutor& operator=(const HipBsgsExecutor&)=delete;
    Ticket submit(const core::BsgsBatch& batch);
    bool poll(Ticket ticket);
    BsgsSearchResult take(Ticket ticket);
    void drain();
    float table_upload_ms() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::backend
