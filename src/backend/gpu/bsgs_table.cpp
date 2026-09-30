#include "keyhunt/backend/gpu_bsgs_table.h"
#include "runtime.h"
#include "bsgs_probe.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace keyhunt::backend {
struct GpuBsgsTable::Impl {
    int device;
    const bsgs::Table& host;
    BsgsUploadOptions options;
    bsgs::View view;
    bsgs::Entry* entries=nullptr;
    uint64_t *offsets=nullptr,*bloom=nullptr;
    bsgs::Key *device_keys=nullptr,*host_keys=nullptr;
    BsgsProbeHit *device_hits=nullptr,*host_hits=nullptr;
    unsigned long long *device_count=nullptr,*host_count=nullptr;
    gpuStream_t stream=nullptr;
    gpuEvent_t start=nullptr,uploaded=nullptr,kernel_done=nullptr,done=nullptr;
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
        gpu_check(gpuMemGetInfo(&free,&total),"gpuMemGetInfo");
        bsgs::require_device_memory(allocation_bytes,free,options.memory_reserve_bytes);
        try {
            gpu_check(gpuStreamCreateWithFlags(&stream,gpuStreamNonBlocking),"gpuStreamCreateWithFlags");
            gpu_check(gpuEventCreate(&start),"gpuEventCreate(start)");
            gpu_check(gpuEventCreate(&uploaded),"gpuEventCreate(uploaded)");
            gpu_check(gpuEventCreate(&kernel_done),"gpuEventCreate(kernel_done)");
            gpu_check(gpuEventCreate(&done),"gpuEventCreate(done)");
            gpu_check(gpuMalloc(&entries,host.entries().size()*sizeof(*entries)),"gpuMalloc(entries)");
            gpu_check(gpuMalloc(&offsets,host.offsets().size()*sizeof(*offsets)),"gpuMalloc(offsets)");
            gpu_check(gpuMalloc(&bloom,host.bloom().size()*sizeof(*bloom)),"gpuMalloc(bloom)");
            gpu_check(gpuMalloc(&device_keys,key_bytes),"gpuMalloc(keys)");
            gpu_check(gpuMalloc(&device_hits,hit_bytes),"gpuMalloc(hits)");
            gpu_check(gpuMalloc(&device_count,sizeof(*device_count)),"gpuMalloc(count)");
            gpu_check(gpuHostMalloc(&host_keys,key_bytes),"gpuHostMalloc(keys)");
            gpu_check(gpuHostMalloc(&host_hits,hit_bytes),"gpuHostMalloc(hits)");
            gpu_check(gpuHostMalloc(&host_count,sizeof(*host_count)),"gpuHostMalloc(count)");
            gpu_check(gpuEventRecord(start,stream),"gpuEventRecord(prepare_start)");
            gpu_check(gpuMemcpyAsync(entries,host.entries().data(),host.entries().size()*sizeof(*entries),gpuMemcpyHostToDevice,stream),"gpuMemcpyAsync(entries)");
            gpu_check(gpuMemcpyAsync(offsets,host.offsets().data(),host.offsets().size()*sizeof(*offsets),gpuMemcpyHostToDevice,stream),"gpuMemcpyAsync(offsets)");
            gpu_check(gpuMemcpyAsync(bloom,host.bloom().data(),host.bloom().size()*sizeof(*bloom),gpuMemcpyHostToDevice,stream),"gpuMemcpyAsync(bloom)");
            gpu_check(gpuEventRecord(done,stream),"gpuEventRecord(prepare_done)");
            gpu_check(gpuEventSynchronize(done),"gpuEventSynchronize(prepare)");
            gpu_check(gpuEventElapsedTime(&prepared_ms,start,done),"gpuEventElapsedTime(prepare)");
            view={entries,offsets,bloom,host.memory().m,host.memory().buckets,host.memory().bloom_words};
        } catch (...) { release(); throw; }
    }
    ~Impl() {
        int previous=0;
        if (gpuGetDevice(&previous)!=gpuSuccess) return;
        if (gpuSetDevice(device)==gpuSuccess) release();
        (void)gpuSetDevice(previous);
    }
    void release() noexcept {
        if (stream) (void)gpuStreamSynchronize(stream);
        if (host_keys) (void)gpuHostFree(host_keys);
        if (host_hits) (void)gpuHostFree(host_hits);
        if (host_count) (void)gpuHostFree(host_count);
        if (device_keys) (void)gpuFree(device_keys);
        if (device_hits) (void)gpuFree(device_hits);
        if (device_count) (void)gpuFree(device_count);
        if (entries) (void)gpuFree(entries);
        if (offsets) (void)gpuFree(offsets);
        if (bloom) (void)gpuFree(bloom);
        if (done) (void)gpuEventDestroy(done);
        if (kernel_done) (void)gpuEventDestroy(kernel_done);
        if (uploaded) (void)gpuEventDestroy(uploaded);
        if (start) (void)gpuEventDestroy(start);
        if (stream) (void)gpuStreamDestroy(stream);
    }
    void healthy() const { if (failed) throw std::runtime_error("GPU BSGS table failed; recreate the upload before retrying"); }
};
GpuBsgsTable::GpuBsgsTable(int device,const bsgs::Table& table,BsgsUploadOptions options)
    : impl_(std::make_unique<Impl>(device,table,options)) {}
GpuBsgsTable::~GpuBsgsTable()=default;
bsgs::View GpuBsgsTable::device_view() const { impl_->healthy(); return impl_->view; }
uint64_t GpuBsgsTable::device_bytes() const { return impl_->allocation_bytes; }
uint64_t GpuBsgsTable::pinned_bytes() const { return impl_->pinned; }
float GpuBsgsTable::preparation_upload_ms() const { return impl_->prepared_ms; }
BsgsProbeResult GpuBsgsTable::probe(const std::vector<bsgs::Key>& keys,bool use_filter) {
    auto& s=*impl_; s.healthy();
    if (keys.empty() || keys.size()>s.options.max_queries) throw std::invalid_argument("BSGS query count exceeds the bounded slot");
    const auto begin=std::chrono::steady_clock::now();
    try {
        DeviceScope selected(s.device);
        const size_t hit_bytes=(keys.size()+1)*sizeof(BsgsProbeHit);
        std::copy(keys.begin(),keys.end(),s.host_keys);
        gpu_check(gpuMemsetAsync(s.device_hits,0xa5,hit_bytes,s.stream),"gpuMemsetAsync(hits)");
        gpu_check(gpuMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"gpuMemsetAsync(count)");
        gpu_check(gpuEventRecord(s.start,s.stream),"gpuEventRecord(start)");
        gpu_check(gpuMemcpyAsync(s.device_keys,s.host_keys,keys.size()*sizeof(bsgs::Key),gpuMemcpyHostToDevice,s.stream),"gpuMemcpyAsync(keys)");
        gpu_check(gpuEventRecord(s.uploaded,s.stream),"gpuEventRecord(uploaded)");
        (void)gpuGetLastError();
        gpuLaunchKernelGGL(gpu::bsgs_probe,dim3((keys.size()+127)/128),dim3(128),0,s.stream,
            s.view,s.device_keys,keys.size(),s.device_hits,s.device_count,use_filter);
        gpu_check(gpuGetLastError(),"bsgs_probe launch");
        gpu_check(gpuEventRecord(s.kernel_done,s.stream),"gpuEventRecord(kernel_done)");
        gpu_check(gpuMemcpyAsync(s.host_hits,s.device_hits,hit_bytes,gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(hits)");
        gpu_check(gpuMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(count)");
        gpu_check(gpuEventRecord(s.done,s.stream),"gpuEventRecord(done)");
        gpu_check(gpuEventSynchronize(s.done),"gpuEventSynchronize(probe)");
#ifdef KEYHUNT_TEST_GPU_FAILURES
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
        gpu_check(gpuEventElapsedTime(&result.upload_ms,s.start,s.uploaded),"gpuEventElapsedTime(upload)");
        gpu_check(gpuEventElapsedTime(&result.kernel_ms,s.uploaded,s.kernel_done),"gpuEventElapsedTime(kernel)");
        gpu_check(gpuEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"gpuEventElapsedTime(download)");
        result.wall_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        return result;
    } catch (...) { s.failed=true; throw; }
}
} // namespace keyhunt::backend
