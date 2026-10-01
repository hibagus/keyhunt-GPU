#pragma once
#include "device_runtime.h"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
// All executor types share one ticket namespace: a diagnostic ticket must never
// authorize consumption of a search result (or vice versa).
inline std::atomic<uint64_t> next_executor_id{1};
#ifdef KEYHUNT_TEST_GPU_FAILURES
// Only the separately compiled fault-test executable contains this hook. It
// fails after the real call so queued operations and partial allocations still
// exercise cleanup; it does not reset devices or induce a hardware fault.
inline thread_local const char* injected_failure = nullptr;
inline thread_local const char* xpoint_test_corruption = nullptr;
inline thread_local const char* hash160_test_corruption = nullptr;
inline thread_local const char* ethereum_test_corruption = nullptr;
inline thread_local const char* vanity_test_corruption = nullptr;
inline thread_local const char* bsgs_test_corruption = nullptr;
inline thread_local const char* bsgs_search_test_corruption = nullptr;
#endif
inline void gpu_check(gpuError_t status, const char* operation) {
#ifdef KEYHUNT_TEST_GPU_FAILURES
    if (injected_failure && std::string(operation) == injected_failure) {
        injected_failure = nullptr;
        throw std::runtime_error(std::string(operation) + ": injected GPU failure");
    }
#endif
    if (status != gpuSuccess)
        throw std::runtime_error(std::string(operation) + ": " + gpuGetErrorName(status) +
                                 " (" + gpuGetErrorString(status) + ")");
}
// Restore the calling thread's selection so discovery and independent executors
// do not silently redirect another component's allocations or launches.
class DeviceScope {
public:
    explicit DeviceScope(int device) {
        gpu_check(gpuGetDevice(&previous_), "gpuGetDevice");
        try {
            gpu_check(gpuSetDevice(device), "gpuSetDevice");
        } catch (...) {
            // A throwing constructor has no destructor. Restore even when a
            // post-call failure occurs after the runtime changed the selection.
            (void)gpuSetDevice(previous_);
            throw;
        }
    }
    ~DeviceScope() { (void)gpuSetDevice(previous_); }
    DeviceScope(const DeviceScope&) = delete;
    DeviceScope& operator=(const DeviceScope&) = delete;
private:
    int previous_ = 0;
};
}
