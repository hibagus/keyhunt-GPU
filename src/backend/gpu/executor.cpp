#include "keyhunt/backend/gpu_executor.h"
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
struct GpuDiagnosticExecutor::Impl {
    int device;
    ExecutorOptions options;
    uint64_t id = next_executor_id.fetch_add(1), sequence = 0;
    bool failed = false;
    std::optional<scheduler::KernelBatch> batch;
    gpuStream_t stream = nullptr;
    gpuEvent_t start = nullptr, kernel_done = nullptr, done = nullptr;
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
        gpu_check(gpuMemGetInfo(&free, &total), "gpuMemGetInfo");
        if (options.memory_reserve_bytes > free ||
            output_bytes + sizeof(*device_count) > free - options.memory_reserve_bytes)
            throw std::runtime_error("insufficient GPU memory after reserved headroom");
        try {
            gpu_check(gpuStreamCreateWithFlags(&stream, gpuStreamNonBlocking), "gpuStreamCreateWithFlags");
            gpu_check(gpuEventCreate(&start), "gpuEventCreate(start)");
            gpu_check(gpuEventCreate(&kernel_done), "gpuEventCreate(kernel_done)");
            gpu_check(gpuEventCreate(&done), "gpuEventCreate(done)");
            gpu_check(gpuMalloc(&device_output, output_bytes), "gpuMalloc(output)");
            gpu_check(gpuMalloc(&device_count, sizeof(*device_count)), "gpuMalloc(count)");
            gpu_check(gpuHostMalloc(&host_output, output_bytes), "gpuHostMalloc(output)");
            gpu_check(gpuHostMalloc(&host_count, sizeof(*host_count)), "gpuHostMalloc(count)");
        } catch (...) {
            release(); // constructors must also release partially acquired resources
            throw;
        }
    }
    ~Impl() {
        // Destructors cannot report runtime errors. Public drain/poll/take do.
        int previous = 0;
        if (gpuGetDevice(&previous) != gpuSuccess) return;
        if (gpuSetDevice(device) == gpuSuccess) release();
        (void)gpuSetDevice(previous);
    }
    void release() noexcept {
        // Keep pinned buffers alive until every queued transfer stops using them.
        // Never reset the whole device (other executors may still be healthy).
        if (stream) (void)gpuStreamSynchronize(stream);
        if (host_count) (void)gpuHostFree(host_count);
        if (host_output) (void)gpuHostFree(host_output);
        if (device_count) (void)gpuFree(device_count);
        if (device_output) (void)gpuFree(device_output);
        if (done) (void)gpuEventDestroy(done);
        if (kernel_done) (void)gpuEventDestroy(kernel_done);
        if (start) (void)gpuEventDestroy(start);
        if (stream) (void)gpuStreamDestroy(stream);
    }
    void healthy() const {
        if (failed) throw std::runtime_error("GPU executor failed; no diagnostic result is valid; recreate to retry");
    }
    void validate(Ticket ticket) const {
        healthy();
        if (!batch || ticket.executor != id || ticket.sequence != sequence)
            throw std::invalid_argument("stale or foreign GPU ticket");
    }
};

GpuDiagnosticExecutor::GpuDiagnosticExecutor(int device, ExecutorOptions options)
    : impl_(std::make_unique<Impl>(device, options)) {}
GpuDiagnosticExecutor::~GpuDiagnosticExecutor() = default;

Ticket GpuDiagnosticExecutor::submit(const scheduler::KernelBatch& batch) {
    auto& s = *impl_;
    s.healthy();
    if (s.batch) throw std::logic_error("GPU result slot busy; take its result before submitting");
    if (batch.step_count() > s.options.max_steps)
        throw std::invalid_argument("batch exceeds GPU executor capacity");
    if (s.sequence == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("GPU ticket sequence exhausted");
    s.batch = batch; // own a snapshot until take() releases the slot
    ++s.sequence;
    s.submitted = Clock::now();
    try {
        DeviceScope selected(s.device);
        DiagnosticScalar begin{};
        const auto bytes = batch.interval().begin().bytes();
        std::copy(bytes.begin(), bytes.end(), begin.bytes);
        gpu_check(gpuMemsetAsync(s.device_output, 0xa5, s.output_bytes, s.stream), "gpuMemsetAsync(output)");
        gpu_check(gpuMemsetAsync(s.device_count, 0, sizeof(*s.device_count), s.stream), "gpuMemsetAsync(count)");
        gpu_check(gpuEventRecord(s.start, s.stream), "gpuEventRecord(start)");
        // Cover the exact batch independently of the device's CU/XCC count.
        // The runtime schedules these workgroups across the selected partition.
        const auto blocks = static_cast<unsigned>((batch.step_count() + 255) / 256);
        // GPU retains a thread-local last error even after the caller handled
        // a failed API call (e.g. invalid device selection). All setup calls
        // above are checked directly; clear stale status before this launch.
        (void)gpuGetLastError();
        gpuLaunchKernelGGL(diagnostic_indices, dim3(blocks), dim3(256), 0, s.stream,
                          begin, batch.step_count(), s.device_output, s.device_count);
        gpu_check(gpuGetLastError(), "diagnostic_indices launch");
        gpu_check(gpuEventRecord(s.kernel_done, s.stream), "gpuEventRecord(kernel_done)");
        const size_t bytes_to_copy = (batch.step_count() + 1) * sizeof(DiagnosticScalar);
        gpu_check(gpuMemcpyAsync(s.host_output, s.device_output, bytes_to_copy,
                                gpuMemcpyDeviceToHost, s.stream), "gpuMemcpyAsync(output)");
        gpu_check(gpuMemcpyAsync(s.host_count, s.device_count, sizeof(*s.device_count),
                                gpuMemcpyDeviceToHost, s.stream), "gpuMemcpyAsync(count)");
        gpu_check(gpuEventRecord(s.done, s.stream), "gpuEventRecord(done)");
        return {s.id, s.sequence};
    } catch (...) { s.failed = true; throw; }
}
bool GpuDiagnosticExecutor::poll(Ticket ticket) {
    auto& s = *impl_;
    s.validate(ticket);
    try {
        DeviceScope selected(s.device);
        const auto status = gpuEventQuery(s.done);
        if (status == gpuErrorNotReady) return false;
        gpu_check(status, "gpuEventQuery");
        return true;
    } catch (...) { s.failed = true; throw; }
}
DiagnosticResult GpuDiagnosticExecutor::take(Ticket ticket) {
    auto& s = *impl_;
    if (!poll(ticket)) throw std::logic_error("GPU result is not ready");
    try {
        DeviceScope selected(s.device);
        DiagnosticResult result{*s.batch, {}};
        result.device_steps = *s.host_count;
        result.launch_count = 1;
        result.device_allocation_bytes = s.output_bytes + sizeof(*s.device_count);
        result.pinned_allocation_bytes = result.device_allocation_bytes;
        result.download_bytes = (s.batch->step_count() + 1) * sizeof(DiagnosticScalar) + sizeof(*s.device_count);
        gpu_check(gpuEventElapsedTime(&result.kernel_ms, s.start, s.kernel_done), "gpuEventElapsedTime(kernel)");
        gpu_check(gpuEventElapsedTime(&result.download_ms, s.kernel_done, s.done), "gpuEventElapsedTime(download)");
        const auto verify_start = Clock::now();
        if (result.device_steps != s.batch->step_count())
            throw std::runtime_error("GPU diagnostic executed-count mismatch");
        for (uint8_t byte : s.host_output[result.device_steps].bytes)
            if (byte != 0xa5) throw std::runtime_error("GPU diagnostic tail guard overwritten");
        result.scalars.reserve(result.device_steps);
        for (uint64_t i = 0; i < result.device_steps; ++i) {
            core::UInt256::Bytes bytes{};
            std::copy_n(s.host_output[i].bytes, bytes.size(), bytes.begin());
            const auto scalar = core::UInt256::from_bytes(bytes);
            if (scalar != s.batch->scalar_at(i))
                throw std::runtime_error("GPU diagnostic scalar mismatch at index " + std::to_string(i));
            result.scalars.push_back(scalar);
        }
        result.verification_ms = milliseconds(Clock::now() - verify_start);
        result.wall_ms = milliseconds(Clock::now() - s.submitted);
        s.batch.reset(); // release only after transfer and verification have succeeded
        return result;
    } catch (...) { s.failed = true; throw; }
}
void GpuDiagnosticExecutor::drain() {
    auto& s = *impl_;
    s.healthy();
    try {
        DeviceScope selected(s.device);
        gpu_check(gpuStreamSynchronize(s.stream), "gpuStreamSynchronize");
    } catch (...) { s.failed = true; throw; }
}
}
