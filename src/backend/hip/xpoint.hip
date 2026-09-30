#include "keyhunt/backend/hip_xpoint.h"
#include "runtime.h"
#include "xpoint.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace keyhunt::backend {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) { return std::chrono::duration<double,std::milli>(value).count(); }
}
struct HipXPointExecutor::Impl {
    int device;
    core::XPointTargets targets;
    const core::XPointVerifier& verifier;
    XPointOptions options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<scheduler::KernelBatch> batch;
    hipStream_t stream = nullptr;
    hipEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
    gpu::Field* device_targets = nullptr;
    core::XPointCandidate *device_output = nullptr, *host_output = nullptr;
    gpu::XPointCounters *device_count = nullptr, *host_count = nullptr;
    size_t output_bytes = 0, target_bytes = 0;
    Clock::time_point submitted;

    Impl(int ordinal, core::XPointTargets values, const core::XPointVerifier& cpu, XPointOptions config)
        : device(ordinal), targets(std::move(values)), verifier(cpu), options(config) {
        if (!options.max_steps || options.max_steps > 1048576)
            throw std::invalid_argument("xpoint max_steps must be in [1, 1048576]");
        if (!options.candidate_capacity || options.candidate_capacity > 1048576)
            throw std::invalid_argument("xpoint candidate capacity must be in [1, 1048576]");
        output_bytes = (uint64_t(options.candidate_capacity)+1)*sizeof(*device_output);
        target_bytes = targets.values().size()*sizeof(gpu::Field);
        DeviceScope selected(device);
        size_t free = 0, total = 0;
        hip_check(hipMemGetInfo(&free,&total),"hipMemGetInfo");
        // Query the selected logical device, never multiply by a package/partition
        // count. Concurrent owners can still consume memory; each allocation checks.
        if (options.memory_reserve_bytes > free ||
            output_bytes+target_bytes+sizeof(*device_count) > free-options.memory_reserve_bytes)
            throw std::runtime_error("insufficient HIP memory after reserved headroom");
        try {
            hip_check(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking),"hipStreamCreateWithFlags");
            hip_check(hipEventCreate(&start),"hipEventCreate(start)");
            hip_check(hipEventCreate(&kernel_done),"hipEventCreate(kernel_done)");
            hip_check(hipEventCreate(&done),"hipEventCreate(done)");
            hip_check(hipMalloc(&device_targets,target_bytes),"hipMalloc(targets)");
            hip_check(hipMalloc(&device_output,output_bytes),"hipMalloc(output)");
            hip_check(hipMalloc(&device_count,sizeof(*device_count)),"hipMalloc(count)");
            hip_check(hipHostMalloc(&host_output,output_bytes),"hipHostMalloc(output)");
            hip_check(hipHostMalloc(&host_count,sizeof(*host_count)),"hipHostMalloc(count)");
            std::vector<gpu::Field> upload;
            upload.reserve(targets.values().size());
            for (const auto& bytes : targets.values()) {
                gpu::Field value;
                if (!gpu::from_bytes_checked(value,bytes.data())) throw std::logic_error("noncanonical prepared target");
                upload.push_back(value);
            }
            // Preparation is synchronous once per immutable target set. Submitted
            // batches use only this executor's nonblocking stream and pinned buffers.
            hip_check(hipMemcpy(device_targets,upload.data(),target_bytes,hipMemcpyHostToDevice),"hipMemcpy(targets)");
        } catch (...) { release(); throw; }
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
        if (done) (void)hipEventDestroy(done);
        if (kernel_done) (void)hipEventDestroy(kernel_done);
        if (start) (void)hipEventDestroy(start);
        if (stream) (void)hipStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("HIP xpoint executor failed; recreate to retry the uncommitted batch");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign HIP ticket");
    }
};
HipXPointExecutor::HipXPointExecutor(int device, core::XPointTargets targets,
    const core::XPointVerifier& verifier, XPointOptions options)
    : impl_(std::make_unique<Impl>(device,std::move(targets),verifier,options)) {}
HipXPointExecutor::~HipXPointExecutor() = default;
Ticket HipXPointExecutor::submit(const scheduler::KernelBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("HIP result slot busy; take its result before submitting");
    if (batch.step_count() > s.options.max_steps) throw std::invalid_argument("batch exceeds HIP executor capacity");
    if (batch.work().identity().target_digest != s.targets.digest())
        throw std::invalid_argument("xpoint target digest does not match the plan");
    if (s.sequence == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("HIP ticket sequence exhausted");
    s.batch = batch;
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        const auto bytes = batch.interval().begin().bytes();
        const auto begin = gpu::scalar_from_bytes(bytes.data());
        hip_check(hipMemsetAsync(s.device_output,0xa5,s.output_bytes,s.stream),"hipMemsetAsync(output)");
        hip_check(hipMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"hipMemsetAsync(count)");
        hip_check(hipEventRecord(s.start,s.stream),"hipEventRecord(start)");
        (void)hipGetLastError();
        hipLaunchKernelGGL(gpu::xpoint_direct,dim3((batch.step_count()+127)/128),dim3(128),0,s.stream,
            begin,batch.step_count(),s.device_targets,uint32_t(s.targets.values().size()),
            s.device_output,s.options.candidate_capacity,s.device_count);
        hip_check(hipGetLastError(),"xpoint launch");
        hip_check(hipEventRecord(s.kernel_done,s.stream),"hipEventRecord(kernel_done)");
        hip_check(hipMemcpyAsync(s.host_output,s.device_output,s.output_bytes,hipMemcpyDeviceToHost,s.stream),"hipMemcpyAsync(output)");
        hip_check(hipMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),hipMemcpyDeviceToHost,s.stream),"hipMemcpyAsync(count)");
        hip_check(hipEventRecord(s.done,s.stream),"hipEventRecord(done)");
        return {s.id,s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool HipXPointExecutor::poll(Ticket ticket) {
    auto& s = *impl_;
    s.validate(ticket);
    try {
        DeviceScope selected(s.device);
        const auto status = hipEventQuery(s.done);
        if (status == hipErrorNotReady) return false;
        hip_check(status,"hipEventQuery");
        return true;
    } catch (...) { s.failed = true; throw; }
}
XPointResult HipXPointExecutor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("HIP result is not ready");
    try {
        DeviceScope selected(s.device);
        XPointResult result{*s.batch,{}};
#ifdef KEYHUNT_TEST_HIP_FAILURES
        // This block is compiled only into the dedicated fault-test executable.
        if (xpoint_test_corruption) {
            const std::string fault = xpoint_test_corruption;
            xpoint_test_corruption = nullptr;
            if (fault == "steps") s.host_count->steps = 0;
            if (fault == "count") s.host_count->candidates = s.batch->step_count()+1;
            if (fault == "overflow") s.host_count->overflow = 1;
            if (fault == "guard") s.host_output[s.options.candidate_capacity].offset = 0;
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
        hip_check(hipEventElapsedTime(&result.kernel_ms,s.start,s.kernel_done),"hipEventElapsedTime(kernel)");
        hip_check(hipEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"hipEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        const auto* guard = reinterpret_cast<const unsigned char*>(s.host_output+s.options.candidate_capacity);
        if (!std::all_of(guard,guard+sizeof(*s.host_output),[](unsigned char c){return c==0xa5;}))
            throw std::runtime_error("HIP xpoint candidate guard overwritten");
        if (counters.invalid || counters.steps != s.batch->step_count() || counters.candidates > counters.steps ||
            counters.overflow > 1 || result.overflow != (counters.candidates > s.options.candidate_capacity))
            throw std::runtime_error("HIP xpoint execution counters are inconsistent");
        if (!result.overflow) {
            result.matches = s.verifier.verify(*s.batch,s.targets,
                std::vector<core::XPointCandidate>(s.host_output,s.host_output+counters.candidates));
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
void HipXPointExecutor::drain() {
    auto& s = *impl_;
    s.healthy();
    try { DeviceScope selected(s.device); hip_check(hipStreamSynchronize(s.stream),"hipStreamSynchronize"); }
    catch (...) { s.failed = true; throw; }
}
} // namespace keyhunt::backend
