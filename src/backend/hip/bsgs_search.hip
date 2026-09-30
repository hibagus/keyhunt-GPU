#include "keyhunt/backend/hip_bsgs.h"
#include "keyhunt/backend/hip_bsgs_table.h"
#include "runtime.h"
#include "bsgs_search.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace keyhunt::backend {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) { return std::chrono::duration<double,std::milli>(value).count(); }
}
struct HipBsgsExecutor::Impl {
    int device;
    int compute_units=0;
    unsigned active_group=1;
    const bsgs::Table& table;
    std::unique_ptr<HipBsgsTable> prepared;
    core::BsgsPublicKeyTargets targets;
    const core::XPointVerifier& verifier;
    BsgsSearchOptions options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<core::BsgsBatch> batch;
    hipStream_t stream = nullptr;
    hipEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
    gpu::Point* device_targets = nullptr;
    gpu::Point* device_powers = nullptr;
    size_t powers_bytes = 0;
    double seed_ms = 0;
    core::BsgsCandidate *device_output = nullptr, *host_output = nullptr;
    gpu::BsgsCounters *device_count = nullptr, *host_count = nullptr;
    size_t output_bytes = 0, target_bytes = 0;
    Clock::time_point submitted;

    Impl(int ordinal,const bsgs::Table& host,core::BsgsPublicKeyTargets values,const core::XPointVerifier& cpu,BsgsSearchOptions config)
        : device(ordinal),table(host),targets(std::move(values)),verifier(cpu),options(config) {
        if (!options.max_steps || options.max_steps>1048576)
            throw std::invalid_argument("BSGS max_steps must be in [1,1048576]");
        if (!options.candidate_capacity || options.candidate_capacity>65536)
            throw std::invalid_argument("BSGS candidate capacity must be in [1,65536]");
        if (options.group_size!=0 && options.group_size!=1 && options.group_size!=8)
            throw std::invalid_argument("BSGS group size must be auto (0), 1 or 8");
        powers_bytes=20*sizeof(gpu::Point);
        output_bytes=(uint64_t(options.candidate_capacity)+1)*sizeof(*device_output);
        target_bytes=targets.values().size()*sizeof(gpu::Point);
        // Conservative host peak covers the resident table, caller and owned
        // targets, upload staging, copied candidates/results and pinned buffers.
        const uint64_t extra=2*targets.values().size()*(sizeof(gpu::Point)+sizeof(core::UncompressedPublicKey))
            +3*output_bytes+1024*1024;
        if (extra>options.host_memory_bytes || table.memory().host_peak_bytes>options.host_memory_bytes-extra)
            throw std::invalid_argument("BSGS search exceeds host memory budget");
        DeviceScope selected(device);
        try {
            hip_check(hipDeviceGetAttribute(&compute_units,hipDeviceAttributeMultiprocessorCount,device),"bsgs hipDeviceGetAttribute(CUs)");
            if (compute_units<=0) throw std::runtime_error("HIP device has no compute units");
            BsgsUploadOptions upload_options;
            upload_options.max_queries=1;
            upload_options.memory_reserve_bytes=options.memory_reserve_bytes;
            upload_options.host_memory_bytes=options.host_memory_bytes-extra;
            prepared=std::make_unique<HipBsgsTable>(device,table,upload_options);
            size_t free=0,total=0;
            hip_check(hipMemGetInfo(&free,&total),"bsgs hipMemGetInfo(search)");
            bsgs::require_device_memory(output_bytes+target_bytes+powers_bytes+sizeof(*device_count),free,options.memory_reserve_bytes);
            hip_check(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking),"bsgs hipStreamCreateWithFlags");
            hip_check(hipEventCreate(&start),"bsgs hipEventCreate(start)");
            hip_check(hipEventCreate(&kernel_done),"bsgs hipEventCreate(kernel_done)");
            hip_check(hipEventCreate(&done),"bsgs hipEventCreate(done)");
            hip_check(hipMalloc(&device_targets,target_bytes),"bsgs hipMalloc(targets)");
            hip_check(hipMalloc(&device_output,output_bytes),"bsgs hipMalloc(output)");
            hip_check(hipMalloc(&device_count,sizeof(*device_count)),"bsgs hipMalloc(count)");
            hip_check(hipHostMalloc(&host_output,output_bytes),"bsgs hipHostMalloc(output)");
            hip_check(hipHostMalloc(&host_count,sizeof(*host_count)),"bsgs hipHostMalloc(count)");
            std::vector<gpu::Point> upload;
            upload.reserve(targets.values().size());
            for (const auto& pub : targets.values()) upload.push_back(point(pub));
            hip_check(hipMemcpy(device_targets,upload.data(),target_bytes,hipMemcpyHostToDevice),"bsgs hipMemcpy(targets)");
            gpu::Point powers[20];
            for (unsigned bit=0;bit<20;++bit) {
                powers[bit]=seed(core::UInt256(table.memory().m).multiply(core::UInt256::power_of_two(bit)));
                gpu::point_negate(powers[bit],powers[bit]);
            }
            hip_check(hipMalloc(&device_powers,powers_bytes),"bsgs hipMalloc(powers)");
            hip_check(hipMemcpy(device_powers,powers,powers_bytes,hipMemcpyHostToDevice),"bsgs hipMemcpy(powers)");
        } catch (...) { release(); throw; }
    }
    gpu::Point seed(const core::UInt256& scalar) const { return point(verifier.derive(scalar)); }
    static gpu::Point point(const core::UncompressedPublicKey& pub) {
        gpu::Point result;
        if (!gpu::from_bytes_checked(result.x,pub.data()+1) || !gpu::from_bytes_checked(result.y,pub.data()+33))
            throw std::runtime_error("noncanonical CPU seed point");
        result.z = gpu::one();
        return result;
    }
    ~Impl() {
        int previous = 0;
        if (hipGetDevice(&previous) != hipSuccess) return;
        if (hipSetDevice(device) == hipSuccess) release();
        (void)hipSetDevice(previous);
    }
    void release() noexcept {
        if (stream) (void)hipStreamSynchronize(stream);
        if (host_count) (void)hipHostFree(host_count);
        if (host_output) (void)hipHostFree(host_output);
        if (device_count) (void)hipFree(device_count);
        if (device_output) (void)hipFree(device_output);
        if (device_targets) (void)hipFree(device_targets);
        if (device_powers) (void)hipFree(device_powers);
        if (done) (void)hipEventDestroy(done);
        if (kernel_done) (void)hipEventDestroy(kernel_done);
        if (start) (void)hipEventDestroy(start);
        if (stream) (void)hipStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("HIP BSGS executor failed; recreate to retry the uncommitted batch");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign HIP ticket");
    }
};
HipBsgsExecutor::HipBsgsExecutor(int device,const bsgs::Table& table, core::BsgsPublicKeyTargets targets,
    const core::XPointVerifier& verifier, BsgsSearchOptions options)
    : impl_(std::make_unique<Impl>(device,table,std::move(targets),verifier,options)) {}
HipBsgsExecutor::~HipBsgsExecutor() = default;
Ticket HipBsgsExecutor::submit(const core::BsgsBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("HIP result slot busy; take its result before submitting");
    if (batch.steps() > s.options.max_steps) throw std::invalid_argument("batch exceeds HIP executor capacity");
    if (batch.target_digest() != s.targets.digest())
        throw std::invalid_argument("BSGS target digest does not match the plan");
    if (batch.table_checksum()!=s.table.checksum() || batch.m()!=s.table.memory().m)
        throw std::invalid_argument("BSGS table identity mismatch");
    if (batch.first_target()+batch.target_count()>s.targets.values().size())
        throw std::invalid_argument("BSGS target subset exceeds uploaded targets");
    if (s.sequence == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("HIP ticket sequence exhausted");
    s.batch = batch;
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        const auto seed_start=Clock::now();
        auto base=s.seed(batch.interval().begin());
        gpu::point_negate(base,base);
        s.seed_ms = milliseconds(Clock::now()-seed_start);
        hip_check(hipMemsetAsync(s.device_output,0xa5,s.output_bytes,s.stream),"bsgs hipMemsetAsync(output)");
        hip_check(hipMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"bsgs hipMemsetAsync(count)");
        hip_check(hipEventRecord(s.start,s.stream),"bsgs hipEventRecord(start)");
        (void)hipGetLastError();
        // Grouping amortizes inversions but divides available parallelism by
        // eight. Measurements favor one giant for small grids and grouping once
        // blocks cover about a quarter of the visible CUs. This conservative
        // crossover is a measured policy, not a claim of optimal occupancy;
        // explicit overrides remain available for other workloads/partitions.
        const uint64_t grouped_blocks=((batch.giants()+7)/8+127)/128;
        s.active_group=s.options.group_size ? s.options.group_size :
            (4*grouped_blocks*batch.target_count()>=uint64_t(s.compute_units) ? 8U : 1U);
        const uint64_t lanes=(batch.giants()+s.active_group-1)/s.active_group;
        const dim3 blocks((lanes+127)/128,batch.target_count());
        if (s.active_group==1) {
            hipLaunchKernelGGL(gpu::bsgs_search<1>,blocks,dim3(128),0,s.stream,
                base,batch.giants(),batch.last_babies(),s.device_targets,batch.first_target(),s.device_powers,
                s.prepared->device_view(),s.device_output,s.options.candidate_capacity,s.device_count);
        } else {
            hipLaunchKernelGGL(gpu::bsgs_search<8>,blocks,dim3(128),0,s.stream,
                base,batch.giants(),batch.last_babies(),s.device_targets,batch.first_target(),s.device_powers,
                s.prepared->device_view(),s.device_output,s.options.candidate_capacity,s.device_count);
        }
        hip_check(hipGetLastError(),"BSGS launch");
        hip_check(hipEventRecord(s.kernel_done,s.stream),"bsgs hipEventRecord(kernel_done)");
        hip_check(hipMemcpyAsync(s.host_output,s.device_output,s.output_bytes,hipMemcpyDeviceToHost,s.stream),"bsgs hipMemcpyAsync(output)");
        hip_check(hipMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),hipMemcpyDeviceToHost,s.stream),"bsgs hipMemcpyAsync(count)");
        hip_check(hipEventRecord(s.done,s.stream),"bsgs hipEventRecord(done)");
        return {s.id,s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool HipBsgsExecutor::poll(Ticket ticket) {
    auto& s = *impl_;
    s.validate(ticket);
    try {
        DeviceScope selected(s.device);
        const auto status = hipEventQuery(s.done);
        if (status == hipErrorNotReady) return false;
        hip_check(status,"bsgs hipEventQuery");
        return true;
    } catch (...) { s.failed = true; throw; }
}
BsgsSearchResult HipBsgsExecutor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("HIP result is not ready");
    try {
        DeviceScope selected(s.device);
        BsgsSearchResult result{*s.batch,{}};
#ifdef KEYHUNT_TEST_HIP_FAILURES
        // This block is compiled only into the dedicated fault-test executable.
        if (bsgs_search_test_corruption) {
            const std::string fault = bsgs_search_test_corruption;
            bsgs_search_test_corruption = nullptr;
            if (fault == "steps") s.host_count->steps = 0;
            if (fault == "count") s.host_count->candidates = s.batch->target_count()+1;
            if (fault == "overflow") s.host_count->overflow = 1;
            if (fault == "guard") s.host_output[s.options.candidate_capacity].giant = 0;
            if (fault == "giant") s.host_output[0].giant = s.batch->giants();
            if (fault == "baby") s.host_output[0].baby = s.batch->m();
            if (fault == "reserved") s.host_output[0].reserved = 1;
            if (fault == "target") s.host_output[0].target = uint32_t(s.targets.values().size());
            if (fault == "false_match") s.host_output[0].baby = 1;
        }
#endif
        const auto counters = *s.host_count;
        result.group_size = s.active_group;
        result.device_steps = counters.steps;
        result.candidate_count = counters.candidates;
        result.tail_rejections = counters.tail_rejections;
        result.overflow = counters.overflow != 0;
        result.device_allocation_bytes = s.prepared->device_bytes()+s.output_bytes+s.target_bytes+s.powers_bytes+sizeof(counters);
        result.pinned_allocation_bytes = s.prepared->pinned_bytes()+s.output_bytes+sizeof(counters);
        result.download_bytes = s.output_bytes+sizeof(counters);
        hip_check(hipEventElapsedTime(&result.kernel_ms,s.start,s.kernel_done),"bsgs hipEventElapsedTime(kernel)");
        hip_check(hipEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"bsgs hipEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        const auto* guard = reinterpret_cast<const unsigned char*>(s.host_output+s.options.candidate_capacity);
        if (!std::all_of(guard,guard+sizeof(*s.host_output),[](unsigned char c){return c==0xa5;}))
            throw std::runtime_error("HIP BSGS candidate guard overwritten");
        if (counters.invalid || counters.steps != s.batch->steps() || counters.candidates > s.batch->target_count() || counters.tail_rejections > s.batch->target_count() ||
            counters.overflow > 1 || result.overflow != (counters.candidates > s.options.candidate_capacity))
            throw std::runtime_error("HIP BSGS execution counters are inconsistent");
        if (!result.overflow) {
            result.matches = core::verify_bsgs(*s.batch,s.targets,s.verifier,
                std::vector<core::BsgsCandidate>(s.host_output,s.host_output+counters.candidates));
            result.verified_steps = counters.steps;
        }
        // Discard the entire overflowing attempt, including its retained prefix.
        // Replay a smaller target subset; each full point has at most one hit.
        result.seed_ms = s.seed_ms;
        result.verification_ms = milliseconds(Clock::now()-verify_start);
        result.wall_ms = milliseconds(Clock::now()-s.submitted);
        s.batch.reset();
        return result;
    } catch (...) { s.failed = true; throw; }
}
void HipBsgsExecutor::drain() {
    auto& s = *impl_;
    s.healthy();
    try { DeviceScope selected(s.device); hip_check(hipStreamSynchronize(s.stream),"bsgs hipStreamSynchronize"); }
    catch (...) { s.failed = true; throw; }
}
float HipBsgsExecutor::table_upload_ms() const { return impl_->prepared->preparation_upload_ms(); }
} // namespace keyhunt::backend
