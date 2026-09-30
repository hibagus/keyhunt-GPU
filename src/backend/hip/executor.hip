#include "keyhunt/backend/hip_executor.h"
#include "runtime.h"
#include "diagnostic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <optional>

namespace keyhunt::backend {
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration value) {
    return std::chrono::duration<double, std::milli>(value).count();
}
}
struct HipDiagnosticExecutor::Impl {
    int device;
    ExecutorOptions options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<scheduler::KernelBatch> batch;
    hipStream_t stream = nullptr;
    hipEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
    DiagnosticScalar *device_output = nullptr, *host_output = nullptr;
    unsigned long long *device_count = nullptr, *host_count = nullptr;
    size_t output_bytes = 0;
    Clock::time_point submitted;

    Impl(int ordinal, ExecutorOptions config) : device(ordinal), options(config) {
        // Bound both host allocations and grid arithmetic before touching HIP.
        if (options.max_steps == 0 || options.max_steps > 1048576)
            throw std::invalid_argument("diagnostic max_steps must be in [1, 1048576]");
        DeviceScope selected(device);
        output_bytes = (options.max_steps + 1) * sizeof(DiagnosticScalar);
        // Query this visible device on every preparation. CPX/QPX/SPX can
        // expose different budgets; neither package HBM nor a cached partition
        // count is an allocation limit for this executor.
        size_t free = 0, total = 0;
        hip_check(hipMemGetInfo(&free, &total), "hipMemGetInfo");
        if (options.memory_reserve_bytes > free ||
            output_bytes + sizeof(*device_count) > free - options.memory_reserve_bytes)
            throw std::runtime_error("insufficient HIP memory after reserved headroom");
        try {
            hip_check(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking), "hipStreamCreateWithFlags");
            hip_check(hipEventCreate(&start), "hipEventCreate(start)");
            hip_check(hipEventCreate(&kernel_done), "hipEventCreate(kernel_done)");
            hip_check(hipEventCreate(&done), "hipEventCreate(done)");
            hip_check(hipMalloc(&device_output, output_bytes), "hipMalloc(output)");
            hip_check(hipMalloc(&device_count, sizeof(*device_count)), "hipMalloc(count)");
            hip_check(hipHostMalloc(&host_output, output_bytes), "hipHostMalloc(output)");
            hip_check(hipHostMalloc(&host_count, sizeof(*host_count)), "hipHostMalloc(count)");
        } catch (...) {
            release(); // constructors must also release partially acquired resources
            throw;
        }
    }
    ~Impl() {
        // Destructors cannot report runtime errors. Public drain/poll/take do.
        int previous = 0;
        if (hipGetDevice(&previous) != hipSuccess) return;
        if (hipSetDevice(device) == hipSuccess) release();
        (void)hipSetDevice(previous);
    }
    void release() noexcept {
        // Keep pinned buffers alive until every queued transfer stops using them.
        // Never reset the whole device (other executors may still be healthy).
        if (stream) (void)hipStreamSynchronize(stream);
        if (host_count) (void)hipHostFree(host_count);
        if (host_output) (void)hipHostFree(host_output);
        if (device_count) (void)hipFree(device_count);
        if (device_output) (void)hipFree(device_output);
        if (done) (void)hipEventDestroy(done);
        if (kernel_done) (void)hipEventDestroy(kernel_done);
        if (start) (void)hipEventDestroy(start);
        if (stream) (void)hipStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("HIP executor failed; no diagnostic result is valid; recreate to retry");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign HIP ticket");
    }
};

HipDiagnosticExecutor::HipDiagnosticExecutor(int device, ExecutorOptions options)
    : impl_(std::make_unique<Impl>(device, options)) {}
HipDiagnosticExecutor::~HipDiagnosticExecutor() = default;

Ticket HipDiagnosticExecutor::submit(const scheduler::KernelBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("HIP result slot busy; take its result before submitting");
    if (batch.step_count() > s.options.max_steps)
        throw std::invalid_argument("batch exceeds HIP executor capacity");
    if (s.sequence == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("HIP ticket sequence exhausted");
    s.batch = batch; // own a snapshot until take() releases the slot
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        DiagnosticScalar begin{};
        const auto bytes = batch.interval().begin().bytes();
        std::copy(bytes.begin(), bytes.end(), begin.bytes);
        hip_check(hipMemsetAsync(s.device_output, 0xa5, s.output_bytes, s.stream), "hipMemsetAsync(output)");
        hip_check(hipMemsetAsync(s.device_count, 0, sizeof(*s.device_count), s.stream), "hipMemsetAsync(count)");
        hip_check(hipEventRecord(s.start, s.stream), "hipEventRecord(start)");
        // Cover the exact batch independently of the device's CU/XCC count.
        // The runtime schedules these workgroups across the selected partition.
        const auto blocks = static_cast<unsigned>((batch.step_count() + 255) / 256);
        // HIP retains a thread-local last error even after the caller handled
        // a failed API call (e.g. invalid device selection). All setup calls
        // above are checked directly; clear stale status before this launch.
        (void)hipGetLastError();
        hipLaunchKernelGGL(diagnostic_indices, dim3(blocks), dim3(256), 0, s.stream,
                          begin, batch.step_count(), s.device_output, s.device_count);
        hip_check(hipGetLastError(), "diagnostic_indices launch");
        hip_check(hipEventRecord(s.kernel_done, s.stream), "hipEventRecord(kernel_done)");
        const size_t bytes_to_copy = (batch.step_count() + 1) * sizeof(DiagnosticScalar);
        hip_check(hipMemcpyAsync(s.host_output, s.device_output, bytes_to_copy,
                                hipMemcpyDeviceToHost, s.stream), "hipMemcpyAsync(output)");
        hip_check(hipMemcpyAsync(s.host_count, s.device_count, sizeof(*s.device_count),
                                hipMemcpyDeviceToHost, s.stream), "hipMemcpyAsync(count)");
        hip_check(hipEventRecord(s.done, s.stream), "hipEventRecord(done)");
        return {s.id, s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool HipDiagnosticExecutor::poll(Ticket ticket) {
    auto& s = *impl_;
    s.validate(ticket);
    try {
        DeviceScope selected(s.device);
        const auto status = hipEventQuery(s.done);
        if (status == hipErrorNotReady) return false;
        hip_check(status, "hipEventQuery");
        return true;
    } catch (...) { s.failed = true; throw; }
}
DiagnosticResult HipDiagnosticExecutor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("HIP result is not ready");
    try {
        DeviceScope selected(s.device);
        DiagnosticResult result{*s.batch, {}};
        result.device_steps = *s.host_count;
        result.launch_count = 1;
        result.device_allocation_bytes = s.output_bytes + sizeof(*s.device_count);
        result.pinned_allocation_bytes = result.device_allocation_bytes;
        result.download_bytes = (s.batch->step_count() + 1) * sizeof(DiagnosticScalar) + sizeof(*s.device_count);
        hip_check(hipEventElapsedTime(&result.kernel_ms, s.start, s.kernel_done), "hipEventElapsedTime(kernel)");
        hip_check(hipEventElapsedTime(&result.download_ms, s.kernel_done, s.done), "hipEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        if (result.device_steps != s.batch->step_count())
            throw std::runtime_error("HIP diagnostic executed-count mismatch");
        for (uint8_t byte : s.host_output[result.device_steps].bytes)
            if (byte != 0xa5) throw std::runtime_error("HIP diagnostic tail guard overwritten");
        result.scalars.reserve(result.device_steps);
        for (uint64_t i = 0; i < result.device_steps; ++i) {
            core::UInt256::Bytes bytes{};
            std::copy_n(s.host_output[i].bytes, bytes.size(), bytes.begin());
            const auto scalar = core::UInt256::from_bytes(bytes);
            if (scalar != s.batch->scalar_at(i))
                throw std::runtime_error("HIP diagnostic scalar mismatch at index " + std::to_string(i));
            result.scalars.push_back(scalar);
        }
        result.verification_ms = milliseconds(Clock::now() - verify_start);
        result.wall_ms = milliseconds(Clock::now() - s.submitted);
        s.batch.reset(); // release only after transfer and verification have succeeded
        return result;
    } catch (...) { s.failed = true; throw; }
}
void HipDiagnosticExecutor::drain() {
    auto& s = *impl_;
    s.healthy();
    try {
        DeviceScope selected(s.device);
        hip_check(hipStreamSynchronize(s.stream), "hipStreamSynchronize");
    } catch (...) { s.failed = true; throw; }
}
}
