#include "keyhunt/backend/gpu_minikeys.h"
#include "runtime.h"
#include "minikeys.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace keyhunt::backend {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) { return std::chrono::duration<double,std::milli>(value).count(); }
}
struct GpuMinikeysExecutor::Impl {
    int device;
    core::MinikeyTargets targets;
    const core::XPointVerifier& verifier;
    MinikeysOptions options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<scheduler::KernelBatch> batch;
    gpuStream_t stream = nullptr;
    gpuEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
    gpu::MinikeysDeviceTarget* device_targets = nullptr;
    core::XPointCandidate *device_output = nullptr, *host_output = nullptr;
    gpu::MinikeysCounters *device_count = nullptr, *host_count = nullptr;
    size_t output_bytes = 0, target_bytes = 0;
    uint32_t capacity = 0; // Effective allocation bound; requested replay semantics stay unchanged.
    Clock::time_point submitted;

    Impl(int ordinal, core::MinikeyTargets values, const core::XPointVerifier& cpu, MinikeysOptions config)
        : device(ordinal), targets(std::move(values)), verifier(cpu), options(config) {
        if (!options.max_steps || options.max_steps > 1048576)
            throw std::invalid_argument("minikeys max_steps must be in [1, 1048576]");
        if (!options.candidate_capacity || options.candidate_capacity > 1048576)
            throw std::invalid_argument("minikeys candidate capacity must be in [1, 1048576]");
        // Each ordinal can emit one relation per enabled encoding. Invalid
        // check bytes are common, but probability must never bound output size.
        if(options.candidate_capacity<targets.max_matches_per_scalar())
            throw std::invalid_argument("minikeys candidate capacity must fit all enabled encodings");
        capacity=uint32_t(std::min<uint64_t>(options.candidate_capacity,
                                           options.max_steps*targets.max_matches_per_scalar()));
        output_bytes=(uint64_t(capacity)+1)*sizeof(*device_output);
        target_bytes=targets.values().size()*sizeof(gpu::MinikeysDeviceTarget);
        DeviceScope selected(device);
        size_t free = 0, total = 0;
        gpu_check(gpuMemGetInfo(&free,&total),"gpuMemGetInfo");
        // Query the selected logical device, never multiply by a package/partition
        // count. Concurrent owners can still consume memory; each allocation checks.
        if (options.memory_reserve_bytes > free ||
            output_bytes+target_bytes+sizeof(*device_count) > free-options.memory_reserve_bytes)
            throw std::runtime_error("insufficient GPU memory after reserved headroom");
        try {
            gpu_check(gpuStreamCreateWithFlags(&stream,gpuStreamNonBlocking),"gpuStreamCreateWithFlags");
            gpu_check(gpuEventCreate(&start),"gpuEventCreate(start)");
            gpu_check(gpuEventCreate(&kernel_done),"gpuEventCreate(kernel_done)");
            gpu_check(gpuEventCreate(&done),"gpuEventCreate(done)");
            gpu_check(gpuMalloc(&device_targets,target_bytes),"gpuMalloc(targets)");
            gpu_check(gpuMalloc(&device_output,output_bytes),"gpuMalloc(output)");
            gpu_check(gpuMalloc(&device_count,sizeof(*device_count)),"gpuMalloc(count)");
            gpu_check(gpuHostMalloc(&host_output,output_bytes),"gpuHostMalloc(output)");
            gpu_check(gpuHostMalloc(&host_count,sizeof(*host_count)),"gpuHostMalloc(count)");
            static_assert(sizeof(gpu::MinikeysDeviceTarget)==sizeof(core::MinikeyTarget));
            // Immutable canonical target bytes are prepared once per owner.
            gpu_check(gpuMemcpy(device_targets,targets.values().data(),target_bytes,gpuMemcpyHostToDevice),"gpuMemcpy(targets)");
        } catch (...) { release(); throw; }
    }
    ~Impl() {
        int previous = 0;
        if (gpuGetDevice(&previous) != gpuSuccess) return;
        if (gpuSetDevice(device) == gpuSuccess) release();
        (void)gpuSetDevice(previous);
    }
    void release() noexcept {
        if (stream) (void)gpuStreamSynchronize(stream);
        if (host_count) (void)gpuHostFree(host_count);
        if (host_output) (void)gpuHostFree(host_output);
        if (device_count) (void)gpuFree(device_count);
        if (device_output) (void)gpuFree(device_output);
        if (device_targets) (void)gpuFree(device_targets);
        if (done) (void)gpuEventDestroy(done);
        if (kernel_done) (void)gpuEventDestroy(kernel_done);
        if (start) (void)gpuEventDestroy(start);
        if (stream) (void)gpuStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("GPU minikeys executor failed; recreate to retry the uncommitted batch");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign GPU ticket");
    }
};
GpuMinikeysExecutor::GpuMinikeysExecutor(int device, core::MinikeyTargets targets,
    const core::XPointVerifier& verifier, MinikeysOptions options)
    : impl_(std::make_unique<Impl>(device,std::move(targets),verifier,options)) {}
GpuMinikeysExecutor::~GpuMinikeysExecutor() = default;
Ticket GpuMinikeysExecutor::submit(const scheduler::KernelBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("GPU result slot busy; take its result before submitting");
    if (batch.step_count() > s.options.max_steps) throw std::invalid_argument("batch exceeds GPU executor capacity");
    if (batch.work().identity().algorithm != scheduler::WorkAlgorithm::DirectMinikeysV1 ||
        batch.work().identity().target_digest != s.targets.digest())
        throw std::invalid_argument("minikeys target digest does not match the plan");
    s.targets.validate_interval(batch.interval());
    if (s.sequence == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("GPU ticket sequence exhausted");
    s.batch = batch;
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        const auto bytes = batch.interval().begin().bytes();
        const auto begin = gpu::scalar_from_bytes(bytes.data());
        gpu_check(gpuMemsetAsync(s.device_output,0xa5,s.output_bytes,s.stream),"gpuMemsetAsync(output)");
        gpu_check(gpuMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"gpuMemsetAsync(count)");
        gpu_check(gpuEventRecord(s.start,s.stream),"gpuEventRecord(start)");
        (void)gpuGetLastError();
        gpuLaunchKernelGGL(gpu::minikeys_direct,dim3((batch.step_count()+127)/128),dim3(128),0,s.stream,
            begin,batch.step_count(),s.targets.length(),s.targets.encodings(),s.device_targets,uint32_t(s.targets.values().size()),
            s.device_output,s.capacity,s.device_count);
        gpu_check(gpuGetLastError(),"minikeys launch");
        gpu_check(gpuEventRecord(s.kernel_done,s.stream),"gpuEventRecord(kernel_done)");
        gpu_check(gpuMemcpyAsync(s.host_output,s.device_output,s.output_bytes,gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(output)");
        gpu_check(gpuMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(count)");
        gpu_check(gpuEventRecord(s.done,s.stream),"gpuEventRecord(done)");
        return {s.id,s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool GpuMinikeysExecutor::poll(Ticket ticket) {
    auto& s = *impl_;
    s.validate(ticket);
    try {
        DeviceScope selected(s.device);
        const auto status = gpuEventQuery(s.done);
        if (status == gpuErrorNotReady) return false;
        gpu_check(status,"gpuEventQuery");
        return true;
    } catch (...) { s.failed = true; throw; }
}
MinikeysResult GpuMinikeysExecutor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("GPU result is not ready");
    try {
        DeviceScope selected(s.device);
        MinikeysResult result{*s.batch,{}};
#ifdef KEYHUNT_TEST_GPU_FAILURES
        // This block is compiled only into the dedicated fault-test executable.
        if (minikeys_test_corruption) {
            const std::string fault = minikeys_test_corruption;
            minikeys_test_corruption = nullptr;
            if (fault == "steps") s.host_count->steps = 0;
            if (fault == "count") s.host_count->candidates = s.batch->step_count()*s.targets.max_matches_per_scalar()+1;
            if (fault == "overflow") s.host_count->overflow = 1;
            if (fault == "guard") s.host_output[s.capacity].offset = 0;
            if (fault == "offset") s.host_output[0].offset = s.batch->step_count();
            if (fault == "target") s.host_output[0].target = uint32_t(s.targets.values().size());
            if (fault == "false_match") s.host_output[0].offset = 1;
        }
#endif
        const auto counters = *s.host_count;
        result.device_steps = counters.steps;
        result.candidate_count = counters.candidates;
        result.overflow = counters.overflow != 0;
        result.device_allocation_bytes = s.output_bytes+s.target_bytes+sizeof(counters);
        result.pinned_allocation_bytes = s.output_bytes+sizeof(counters);
        result.download_bytes = result.pinned_allocation_bytes;
        gpu_check(gpuEventElapsedTime(&result.kernel_ms,s.start,s.kernel_done),"gpuEventElapsedTime(kernel)");
        gpu_check(gpuEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"gpuEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        const auto* guard = reinterpret_cast<const unsigned char*>(s.host_output+s.capacity);
        if (!std::all_of(guard,guard+sizeof(*s.host_output),[](unsigned char c){return c==0xa5;}))
            throw std::runtime_error("GPU minikeys candidate guard overwritten");
        if (counters.invalid || counters.steps != s.batch->step_count() || counters.candidates > counters.steps*s.targets.max_matches_per_scalar() ||
            counters.overflow > 1 || result.overflow != (counters.candidates > s.capacity))
            throw std::runtime_error("GPU minikeys execution counters are inconsistent");
        if (!result.overflow) {
            result.matches = core::verify_minikeys(*s.batch,s.targets,
                std::vector<core::XPointCandidate>(s.host_output,s.host_output+counters.candidates),s.verifier);
            result.verified_steps = counters.steps;
        }
        // Discard the entire overflowing attempt, including its retained prefix.
        // The caller must replay smaller batches before advancing its cursor.
        result.verification_ms = milliseconds(Clock::now()-verify_start);
        result.wall_ms = milliseconds(Clock::now()-s.submitted);
        s.batch.reset();
        return result;
    } catch (...) { s.failed = true; throw; }
}
void GpuMinikeysExecutor::drain() {
    auto& s = *impl_;
    s.healthy();
    try { DeviceScope selected(s.device); gpu_check(gpuStreamSynchronize(s.stream),"gpuStreamSynchronize"); }
    catch (...) { s.failed = true; throw; }
}
} // namespace keyhunt::backend
