#pragma once
#include <hip/hip_runtime.h>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
inline void hip_check(hipError_t status, const char* operation) {
    if (status != hipSuccess)
        throw std::runtime_error(std::string(operation) + ": " + hipGetErrorName(status) +
                                 " (" + hipGetErrorString(status) + ")");
}
// Restore the calling thread's selection so discovery and independent executors
// do not silently redirect another component's allocations or launches.
class HipDiscoveryDeviceScope {
public:
    explicit HipDiscoveryDeviceScope(int device) {
        hip_check(hipGetDevice(&previous_), "hipGetDevice");
        hip_check(hipSetDevice(device), "hipSetDevice");
    }
    ~HipDiscoveryDeviceScope() { (void)hipSetDevice(previous_); }
    HipDiscoveryDeviceScope(const HipDiscoveryDeviceScope&) = delete;
    HipDiscoveryDeviceScope& operator=(const HipDiscoveryDeviceScope&) = delete;
private:
    int previous_ = 0;
};
}
