#include "self_test.h"
#include <cuda.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* name = nullptr;
    cuGetErrorName(result, &name);
    throw std::runtime_error(std::string(operation) + ": " + (name ? name : "unknown CUDA error"));
}

std::vector<int> active_devices(int count) {
    std::vector<int> active;
    for (int ordinal = 0; ordinal < count; ++ordinal) {
        CUdevice device;
        check(cuDeviceGet(&device, ordinal), "cuDeviceGet");
        unsigned flags = 0;
        int initialized = 0;
        // Query the primary context without retaining it or selecting this
        // device: the observer must not create the contexts it is checking.
        check(cuDevicePrimaryCtxGetState(device, &flags, &initialized), "cuDevicePrimaryCtxGetState");
        if (initialized) active.push_back(ordinal);
    }
    return active;
}
}

int main() {
    try {
        check(cuInit(0), "cuInit");
        int count = 0;
        check(cuDeviceGetCount(&count), "cuDeviceGetCount");
        if (count < 2) {
            std::cout << "Context isolation requires at least two visible CUDA devices\n";
            return 77;
        }
        if (!active_devices(count).empty())
            throw std::runtime_error("test process already has an active primary context");

        // Select a nondefault ordinal in a fresh process. This catches both
        // full-inventory discovery and a scope destructor restoring ordinal 0,
        // which can initialize an unrelated context on CUDA 12 and later.
        const int selected = count - 1;
        const auto result = keyhunt::coordination::device_self_test(selected);
        const auto active = active_devices(count);
        if (!result.at("passed").get<bool>() || result.at("ordinal") != selected ||
            active != std::vector<int>{selected})
            throw std::runtime_error("worker self-test initialized an unrelated CUDA device");

        keyhunt::coordination::Json report{{"passed", true}, {"visible_devices", count},
            {"active_devices", active}, {"self_test", result}};
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
