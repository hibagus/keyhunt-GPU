#pragma once
#include <hip/hip_runtime.h>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
#ifdef KEYHUNT_TEST_HIP_FAILURES
// Only the separately compiled fault-test executable contains this hook. It
// fails after the real call so queued operations and partial allocations still
// exercise cleanup; it does not reset devices or induce a hardware fault.
inline thread_local const char* injected_failure = nullptr;
#endif
inline void hip_check(hipError_t status, const char* operation) {
#ifdef KEYHUNT_TEST_HIP_FAILURES
    if (injected_failure && std::string(operation) == injected_failure) {
        injected_failure = nullptr;
        throw std::runtime_error(std::string(operation) + ": injected HIP failure");
    }
#endif
    if (status != hipSuccess)
        throw std::runtime_error(std::string(operation) + ": " + hipGetErrorName(status) +
                                 " (" + hipGetErrorString(status) + ")");
}
// Restore the calling thread's selection so discovery and independent executors
// do not silently redirect another component's allocations or launches.
class DeviceScope {
public:
    explicit DeviceScope(int device) {
        hip_check(hipGetDevice(&previous_), "hipGetDevice");
        hip_check(hipSetDevice(device), "hipSetDevice");
    }
    ~DeviceScope() { (void)hipSetDevice(previous_); }
    DeviceScope(const DeviceScope&) = delete;
    DeviceScope& operator=(const DeviceScope&) = delete;
private:
    int previous_ = 0;
};
}
