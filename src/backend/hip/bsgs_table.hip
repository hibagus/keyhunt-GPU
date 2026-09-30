#include "keyhunt/backend/hip_bsgs_table.h"
#include "runtime.h"
#include "bsgs_probe.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace keyhunt::backend {
struct HipBsgsTable::Impl {
    int device;
    const bsgs::Table& host;
    BsgsUploadOptions options;
    bsgs::View view;
    bsgs::Entry* entries=nullptr;
    uint64_t *offsets=nullptr,*bloom=nullptr;
    bsgs::Key *device_keys=nullptr,*host_keys=nullptr;
    BsgsProbeHit *device_hits=nullptr,*host_hits=nullptr;
    unsigned long long *device_count=nullptr,*host_count=nullptr;
    hipStream_t stream=nullptr;
    hipEvent_t start=nullptr,uploaded=nullptr,kernel_done=nullptr,done=nullptr;
    uint64_t key_bytes=0,hit_bytes=0,allocation_bytes=0,pinned=0;
    float prepared_ms=0;
    bool failed=false;
    Impl(int ordinal,const bsgs::Table& table,BsgsUploadOptions config) : device(ordinal),host(table),options(config) {
        if (!options.max_queries || options.max_queries>65536) throw std::invalid_argument("BSGS max_queries must be in [1,65536]");
        key_bytes=uint64_t(options.max_queries)*sizeof(bsgs::Key);
        hit_bytes=(uint64_t(options.max_queries)+1)*sizeof(BsgsProbeHit);
        pinned=key_bytes+hit_bytes+sizeof(*device_count);
        if (host.memory().resident_bytes>UINT64_MAX-pinned) throw std::overflow_error("BSGS device allocation overflow");
        allocation_bytes=host.memory().resident_bytes+pinned;
        // Pinned buffers, caller query vector and returned result coexist. The
        // immutable host table is borrowed, never duplicated by preparation.
        if (host.memory().resident_bytes>UINT64_MAX-2*pinned-65536 ||
            host.memory().resident_bytes+2*pinned+65536>options.host_memory_bytes)
            throw std::invalid_argument("BSGS upload exceeds the host memory budget");
        DeviceScope selected(device);
        size_t free=0,total=0;
        hip_check(hipMemGetInfo(&free,&total),"hipMemGetInfo");
        bsgs::require_device_memory(allocation_bytes,free,options.memory_reserve_bytes);
        try {
            hip_check(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking),"hipStreamCreateWithFlags");
            hip_check(hipEventCreate(&start),"hipEventCreate(start)");
            hip_check(hipEventCreate(&uploaded),"hipEventCreate(uploaded)");
            hip_check(hipEventCreate(&kernel_done),"hipEventCreate(kernel_done)");
            hip_check(hipEventCreate(&done),"hipEventCreate(done)");
            hip_check(hipMalloc(&entries,host.entries().size()*sizeof(*entries)),"hipMalloc(entries)");
            hip_check(hipMalloc(&offsets,host.offsets().size()*sizeof(*offsets)),"hipMalloc(offsets)");
            hip_check(hipMalloc(&bloom,host.bloom().size()*sizeof(*bloom)),"hipMalloc(bloom)");
            hip_check(hipMalloc(&device_keys,key_bytes),"hipMalloc(keys)");
            hip_check(hipMalloc(&device_hits,hit_bytes),"hipMalloc(hits)");
            hip_check(hipMalloc(&device_count,sizeof(*device_count)),"hipMalloc(count)");
            hip_check(hipHostMalloc(&host_keys,key_bytes),"hipHostMalloc(keys)");
            hip_check(hipHostMalloc(&host_hits,hit_bytes),"hipHostMalloc(hits)");
            hip_check(hipHostMalloc(&host_count,sizeof(*host_count)),"hipHostMalloc(count)");
            hip_check(hipEventRecord(start,stream),"hipEventRecord(prepare_start)");
            hip_check(hipMemcpyAsync(entries,host.entries().data(),host.entries().size()*sizeof(*entries),hipMemcpyHostToDevice,stream),"hipMemcpyAsync(entries)");
            hip_check(hipMemcpyAsync(offsets,host.offsets().data(),host.offsets().size()*sizeof(*offsets),hipMemcpyHostToDevice,stream),"hipMemcpyAsync(offsets)");
            hip_check(hipMemcpyAsync(bloom,host.bloom().data(),host.bloom().size()*sizeof(*bloom),hipMemcpyHostToDevice,stream),"hipMemcpyAsync(bloom)");
            hip_check(hipEventRecord(done,stream),"hipEventRecord(prepare_done)");
            hip_check(hipEventSynchronize(done),"hipEventSynchronize(prepare)");
            hip_check(hipEventElapsedTime(&prepared_ms,start,done),"hipEventElapsedTime(prepare)");
            view={entries,offsets,bloom,host.memory().m,host.memory().buckets,host.memory().bloom_words};
        } catch (...) { release(); throw; }
    }
    ~Impl() {
        int previous=0;
        if (hipGetDevice(&previous)!=hipSuccess) return;
        if (hipSetDevice(device)==hipSuccess) release();
        (void)hipSetDevice(previous);
    }
    void release() noexcept {
        if (stream) (void)hipStreamSynchronize(stream);
        if (host_keys) (void)hipHostFree(host_keys);
        if (host_hits) (void)hipHostFree(host_hits);
        if (host_count) (void)hipHostFree(host_count);
        if (device_keys) (void)hipFree(device_keys);
        if (device_hits) (void)hipFree(device_hits);
        if (device_count) (void)hipFree(device_count);
        if (entries) (void)hipFree(entries);
        if (offsets) (void)hipFree(offsets);
        if (bloom) (void)hipFree(bloom);
        if (done) (void)hipEventDestroy(done);
        if (kernel_done) (void)hipEventDestroy(kernel_done);
        if (uploaded) (void)hipEventDestroy(uploaded);
        if (start) (void)hipEventDestroy(start);
        if (stream) (void)hipStreamDestroy(stream);
    }
    void healthy() const { if (failed) throw std::runtime_error("HIP BSGS table failed; recreate the upload before retrying"); }
};
HipBsgsTable::HipBsgsTable(int device,const bsgs::Table& table,BsgsUploadOptions options)
    : impl_(std::make_unique<Impl>(device,table,options)) {}
HipBsgsTable::~HipBsgsTable()=default;
bsgs::View HipBsgsTable::device_view() const { impl_->healthy(); return impl_->view; }
uint64_t HipBsgsTable::device_bytes() const { return impl_->allocation_bytes; }
uint64_t HipBsgsTable::pinned_bytes() const { return impl_->pinned; }
float HipBsgsTable::preparation_upload_ms() const { return impl_->prepared_ms; }
BsgsProbeResult HipBsgsTable::probe(const std::vector<bsgs::Key>& keys,bool use_filter) {
    auto& s=*impl_; s.healthy();
    if (keys.empty() || keys.size()>s.options.max_queries) throw std::invalid_argument("BSGS query count exceeds the bounded slot");
    const auto begin=std::chrono::steady_clock::now();
    try {
        DeviceScope selected(s.device);
        const size_t hit_bytes=(keys.size()+1)*sizeof(BsgsProbeHit);
        std::copy(keys.begin(),keys.end(),s.host_keys);
        hip_check(hipMemsetAsync(s.device_hits,0xa5,hit_bytes,s.stream),"hipMemsetAsync(hits)");
        hip_check(hipMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"hipMemsetAsync(count)");
        hip_check(hipEventRecord(s.start,s.stream),"hipEventRecord(start)");
        hip_check(hipMemcpyAsync(s.device_keys,s.host_keys,keys.size()*sizeof(bsgs::Key),hipMemcpyHostToDevice,s.stream),"hipMemcpyAsync(keys)");
        hip_check(hipEventRecord(s.uploaded,s.stream),"hipEventRecord(uploaded)");
        (void)hipGetLastError();
        hipLaunchKernelGGL(gpu::bsgs_probe,dim3((keys.size()+127)/128),dim3(128),0,s.stream,
            s.view,s.device_keys,keys.size(),s.device_hits,s.device_count,use_filter);
        hip_check(hipGetLastError(),"bsgs_probe launch");
        hip_check(hipEventRecord(s.kernel_done,s.stream),"hipEventRecord(kernel_done)");
        hip_check(hipMemcpyAsync(s.host_hits,s.device_hits,hit_bytes,hipMemcpyDeviceToHost,s.stream),"hipMemcpyAsync(hits)");
        hip_check(hipMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),hipMemcpyDeviceToHost,s.stream),"hipMemcpyAsync(count)");
        hip_check(hipEventRecord(s.done,s.stream),"hipEventRecord(done)");
        hip_check(hipEventSynchronize(s.done),"hipEventSynchronize(probe)");
#ifdef KEYHUNT_TEST_HIP_FAILURES
        // Mutate completed host copies only in the separate fault-test binary.
        if (bsgs_test_corruption) {
            const std::string fault=bsgs_test_corruption; bsgs_test_corruption=nullptr;
            if (fault=="count") *s.host_count=0;
            if (fault=="guard") s.host_hits[keys.size()].j=0;
            if (fault=="hit") s.host_hits[0].j=UINT64_MAX;
            if (fault=="filter") s.host_hits[0].bloom_positive=0;
        }
#endif
        BsgsProbeResult result;
        result.device_queries=*s.host_count;
        if (result.device_queries!=keys.size()) throw std::runtime_error("BSGS device query count mismatch");
        const auto* guard=reinterpret_cast<const uint8_t*>(s.host_hits+keys.size());
        if (!std::all_of(guard,guard+sizeof(BsgsProbeHit),[](uint8_t b){return b==0xa5;}))
            throw std::runtime_error("BSGS query tail guard overwritten");
        result.hits.assign(s.host_hits,s.host_hits+keys.size());
        for (size_t i=0;i<keys.size();++i) {
            const auto range=bsgs::lookup(s.host.view(),keys[i],false); // independent of the host Bloom gate
            const auto& hit=result.hits[i];
            if (hit.begin!=range.begin || hit.end!=range.end || hit.reserved ||
                hit.j!=(range.begin<range.end ? s.host.entries()[range.begin].j : UINT64_MAX) ||
                hit.bloom_positive!=uint32_t(bsgs::maybe_contains(s.host.view(),keys[i])))
                throw std::runtime_error("BSGS CPU/GPU filter or exact lookup mismatch");
        }
        hip_check(hipEventElapsedTime(&result.upload_ms,s.start,s.uploaded),"hipEventElapsedTime(upload)");
        hip_check(hipEventElapsedTime(&result.kernel_ms,s.uploaded,s.kernel_done),"hipEventElapsedTime(kernel)");
        hip_check(hipEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"hipEventElapsedTime(download)");
        result.wall_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        return result;
    } catch (...) { s.failed=true; throw; }
}
} // namespace keyhunt::backend
