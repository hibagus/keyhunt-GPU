#include "keyhunt/backend/gpu_hash160.h"
#include "runtime.h"
#include "hash160.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace keyhunt::backend {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) { return std::chrono::duration<double,std::milli>(value).count(); }
}
struct GpuHash160Executor::Impl {
    int device;
    core::Hash160Targets targets;
    const core::XPointVerifier& verifier;
    Hash160Options options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<scheduler::KernelBatch> batch;
    gpuStream_t stream = nullptr;
    gpuEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
    gpu::Hash160DeviceTarget* device_targets = nullptr;
    gpu::Hash160Power* device_powers = nullptr;
    size_t powers_bytes = 0;
    double seed_ms = 0;
    core::XPointCandidate *device_output = nullptr, *host_output = nullptr;
    gpu::Hash160Counters *device_count = nullptr, *host_count = nullptr;
    size_t output_bytes = 0, target_bytes = 0;
    uint32_t capacity = 0; // Effective allocation bound; requested replay semantics stay unchanged.
    Clock::time_point submitted;

    Impl(int ordinal, core::Hash160Targets values, const core::XPointVerifier& cpu, Hash160Options config)
        : device(ordinal), targets(std::move(values)), verifier(cpu), options(config) {
        core::validate_scalar_stride(options.stride);
        if (!options.max_steps || options.max_steps > 1048576)
            throw std::invalid_argument("hash160 max_steps must be in [1, 1048576]");
        if (!options.candidate_capacity || options.candidate_capacity > 1048576)
            throw std::invalid_argument("hash160 candidate capacity must be in [1, 1048576]");
        if (options.kernel != XPointKernel::Direct && options.kernel != XPointKernel::Stepped)
            throw std::invalid_argument("unsupported GPU hash160 kernel");
        powers_bytes = options.kernel == XPointKernel::Stepped ? 20*sizeof(gpu::Hash160Power) : 0;
        // Hashes have no two-preimage bound. Each scalar can emit one match
        // per enabled encoding, regardless of how many targets were loaded.
        if(options.candidate_capacity<targets.max_matches_per_scalar())
            throw std::invalid_argument("HASH160 candidate capacity must fit all enabled encodings");
        capacity=uint32_t(std::min<uint64_t>(options.candidate_capacity,
                                           options.max_steps*targets.max_matches_per_scalar()));
        output_bytes=(uint64_t(capacity)+1)*sizeof(*device_output);
        target_bytes=targets.values().size()*sizeof(gpu::Hash160DeviceTarget);
        DeviceScope selected(device);
        size_t free = 0, total = 0;
        gpu_check(gpuMemGetInfo(&free,&total),"gpuMemGetInfo");
        // Query the selected logical device, never multiply by a package/partition
        // count. Concurrent owners can still consume memory; each allocation checks.
        if (options.memory_reserve_bytes > free ||
            output_bytes+target_bytes+powers_bytes+sizeof(*device_count) > free-options.memory_reserve_bytes)
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
            static_assert(sizeof(gpu::Hash160DeviceTarget)==sizeof(core::Hash160Target));
            // Immutable canonical target bytes are prepared once per owner.
            gpu_check(gpuMemcpy(device_targets,targets.values().data(),target_bytes,gpuMemcpyHostToDevice),"gpuMemcpy(targets)");
            if (powers_bytes) {
                // Reverse point steps use n-S; candidate arithmetic itself never wraps.
                // Preserve each backend's measured cache representation.
                gpu::Hash160Power powers[20];
                for (unsigned bit=0;bit<20;++bit) {
                    const auto point = seed(core::scalar_stride_power(options.reverse?core::scalar_order().subtract(options.stride):options.stride,bit));
#if defined(__CUDACC__)
                    powers[bit] = point;
#else
                    powers[bit] = {point.x,point.y,false};
#endif
                }
                gpu_check(gpuMalloc(&device_powers,powers_bytes),"gpuMalloc(powers)");
                gpu_check(gpuMemcpy(device_powers,powers,powers_bytes,gpuMemcpyHostToDevice),"gpuMemcpy(powers)");
            }
        } catch (...) { release(); throw; }
    }
    gpu::Point seed(const core::UInt256& scalar) const {
        const auto pub = verifier.derive(scalar);
        gpu::Point result;
        if (!gpu::from_bytes_checked(result.x,pub.data()+1) || !gpu::from_bytes_checked(result.y,pub.data()+33))
            throw std::runtime_error("noncanonical CPU seed point");
        result.z = gpu::one();
        return result;
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
        if (device_powers) (void)gpuFree(device_powers);
        if (done) (void)gpuEventDestroy(done);
        if (kernel_done) (void)gpuEventDestroy(kernel_done);
        if (start) (void)gpuEventDestroy(start);
        if (stream) (void)gpuStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("GPU hash160 executor failed; recreate to retry the uncommitted batch");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign GPU ticket");
    }
};
GpuHash160Executor::GpuHash160Executor(int device, core::Hash160Targets targets,
    const core::XPointVerifier& verifier, Hash160Options options)
    : impl_(std::make_unique<Impl>(device,std::move(targets),verifier,options)) {}
GpuHash160Executor::~GpuHash160Executor() = default;
Ticket GpuHash160Executor::submit(const scheduler::KernelBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("GPU result slot busy; take its result before submitting");
    if (batch.step_count() > s.options.max_steps) throw std::invalid_argument("batch exceeds GPU executor capacity");
    if (scheduler::scalar_family(batch.work().identity().algorithm) != scheduler::WorkAlgorithm::DirectHash160V1 ||
        batch.work().identity().target_digest != s.targets.digest())
        throw std::invalid_argument("hash160 target digest does not match the plan");
    if(batch.scalar_stride()!=s.options.stride || batch.scalar_reverse()!=s.options.reverse)throw std::invalid_argument("batch stride or order differs from prepared executor");
    if (s.sequence == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("GPU ticket sequence exhausted");
    s.batch = batch;
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        const auto bytes = batch.scalar_at(0).bytes();
        const auto begin = gpu::scalar_from_bytes(bytes.data());
        const auto stride_bytes=s.options.stride.bytes();
        const auto stride=gpu::scalar_from_bytes(stride_bytes.data());
        const auto seed_start = Clock::now();
        const auto base = s.options.kernel == XPointKernel::Stepped ? s.seed(batch.scalar_at(0)) : gpu::Point{};
        s.seed_ms = milliseconds(Clock::now()-seed_start);
        gpu_check(gpuMemsetAsync(s.device_output,0xa5,s.output_bytes,s.stream),"gpuMemsetAsync(output)");
        gpu_check(gpuMemsetAsync(s.device_count,0,sizeof(*s.device_count),s.stream),"gpuMemsetAsync(count)");
        gpu_check(gpuEventRecord(s.start,s.stream),"gpuEventRecord(start)");
        (void)gpuGetLastError();
        if (s.options.kernel == XPointKernel::Direct) {
            // Keep the original forward unit-step specialization. Indexed paths
            // use checked multiply-add (forward) or multiply-subtract (reverse).
            if(s.options.reverse){
                gpuLaunchKernelGGL(gpu::hash160_direct<2>,dim3((batch.step_count()+127)/128),dim3(128),0,s.stream,
                    begin,stride,batch.step_count(),s.targets.encodings(),s.device_targets,uint32_t(s.targets.values().size()),
                    s.device_output,s.capacity,s.device_count);
            }else if(s.options.stride==core::UInt256(1)){
                gpuLaunchKernelGGL(gpu::hash160_direct<false>,dim3((batch.step_count()+127)/128),dim3(128),0,s.stream,
                    begin,stride,batch.step_count(),s.targets.encodings(),s.device_targets,uint32_t(s.targets.values().size()),
                    s.device_output,s.capacity,s.device_count);
            }else{
                gpuLaunchKernelGGL(gpu::hash160_direct<true>,dim3((batch.step_count()+127)/128),dim3(128),0,s.stream,
                    begin,stride,batch.step_count(),s.targets.encodings(),s.device_targets,uint32_t(s.targets.values().size()),
                    s.device_output,s.capacity,s.device_count);
            }
        } else {
            const auto lanes = (batch.step_count()+gpu::hash160_group-1)/gpu::hash160_group;
            const dim3 blocks((lanes+127)/128);
            gpuLaunchKernelGGL(gpu::hash160_stepped,blocks,dim3(128),0,s.stream,
                base,batch.step_count(),s.device_powers,s.targets.encodings(),s.device_targets,
                uint32_t(s.targets.values().size()),s.device_output,s.capacity,s.device_count);
        }
        gpu_check(gpuGetLastError(),"hash160 launch");
        gpu_check(gpuEventRecord(s.kernel_done,s.stream),"gpuEventRecord(kernel_done)");
        gpu_check(gpuMemcpyAsync(s.host_output,s.device_output,s.output_bytes,gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(output)");
        gpu_check(gpuMemcpyAsync(s.host_count,s.device_count,sizeof(*s.device_count),gpuMemcpyDeviceToHost,s.stream),"gpuMemcpyAsync(count)");
        gpu_check(gpuEventRecord(s.done,s.stream),"gpuEventRecord(done)");
        return {s.id,s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool GpuHash160Executor::poll(Ticket ticket) {
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
Hash160Result GpuHash160Executor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("GPU result is not ready");
    try {
        DeviceScope selected(s.device);
        Hash160Result result{*s.batch,{}};
#ifdef KEYHUNT_TEST_GPU_FAILURES
        // This block is compiled only into the dedicated fault-test executable.
        if (hash160_test_corruption) {
            const std::string fault = hash160_test_corruption;
            hash160_test_corruption = nullptr;
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
        result.device_allocation_bytes = s.output_bytes+s.target_bytes+s.powers_bytes+sizeof(counters);
        result.pinned_allocation_bytes = s.output_bytes+sizeof(counters);
        result.download_bytes = result.pinned_allocation_bytes;
        gpu_check(gpuEventElapsedTime(&result.kernel_ms,s.start,s.kernel_done),"gpuEventElapsedTime(kernel)");
        gpu_check(gpuEventElapsedTime(&result.download_ms,s.kernel_done,s.done),"gpuEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        const auto* guard = reinterpret_cast<const unsigned char*>(s.host_output+s.capacity);
        if (!std::all_of(guard,guard+sizeof(*s.host_output),[](unsigned char c){return c==0xa5;}))
            throw std::runtime_error("GPU hash160 candidate guard overwritten");
        if (counters.invalid || counters.steps != s.batch->step_count() || counters.candidates > counters.steps*s.targets.max_matches_per_scalar() ||
            counters.overflow > 1 || result.overflow != (counters.candidates > s.capacity))
            throw std::runtime_error("GPU hash160 execution counters are inconsistent");
        if (!result.overflow) {
            result.matches = core::verify_hash160(*s.batch,s.targets,
                std::vector<core::XPointCandidate>(s.host_output,s.host_output+counters.candidates),s.verifier);
            result.verified_steps = counters.steps;
        }
        // Discard the entire overflowing attempt, including its retained prefix.
        // The caller must replay smaller batches before advancing its cursor.
        result.seed_ms = s.seed_ms;
        result.verification_ms = milliseconds(Clock::now()-verify_start);
        result.wall_ms = milliseconds(Clock::now()-s.submitted);
        s.batch.reset();
        return result;
    } catch (...) { s.failed = true; throw; }
}
void GpuHash160Executor::drain() {
    auto& s = *impl_;
    s.healthy();
    try { DeviceScope selected(s.device); gpu_check(gpuStreamSynchronize(s.stream),"gpuStreamSynchronize"); }
    catch (...) { s.failed = true; throw; }
}
} // namespace keyhunt::backend
