#pragma once
// Discovery-only HIP API double. This header is private to one CPU test target;
// production still compiles against the installed HIP SDK. No kernels are mocked.
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

enum hipError_t { hipSuccess, hipErrorNoDevice, hipErrorInvalidDevice, hipErrorUnknown };
struct hipUUID { char bytes[16]{}; };
struct hipDeviceProp_t {
    char name[256]{}, gcnArchName[256]{};
    int multiProcessorCount = 0, warpSize = 0;
    size_t totalGlobalMem = 0;
};
namespace fake_hip {
struct Device {
    hipDeviceProp_t properties;
    hipUUID uuid;
    std::string bdf;
    size_t total = 0, free = 0;
};
inline std::vector<Device> devices;
inline std::vector<int> memory_queries;
inline int selected = 0, fail_memory_on = -1;
}
inline const char* hipGetErrorName(hipError_t status) {
    return status == hipErrorInvalidDevice ? "hipErrorInvalidDevice" : "hipErrorUnknown";
}
inline const char* hipGetErrorString(hipError_t) { return "simulated discovery failure"; }
inline hipError_t hipGetDevice(int* device) { *device = fake_hip::selected; return hipSuccess; }
inline hipError_t hipSetDevice(int device) {
    if (device < 0 || size_t(device) >= fake_hip::devices.size()) return hipErrorInvalidDevice;
    fake_hip::selected = device;
    return hipSuccess;
}
inline hipError_t hipRuntimeGetVersion(int* version) { *version = 71500000; return hipSuccess; }
inline hipError_t hipDriverGetVersion(int* version) { *version = 71500000; return hipSuccess; }
inline hipError_t hipGetDeviceCount(int* count) {
    *count = int(fake_hip::devices.size());
    return *count ? hipSuccess : hipErrorNoDevice;
}
inline hipError_t hipGetDeviceProperties(hipDeviceProp_t* prop, int device) {
    *prop = fake_hip::devices.at(device).properties;
    return hipSuccess;
}
inline hipError_t hipMemGetInfo(size_t* free, size_t* total) {
    const int device = fake_hip::selected;
    fake_hip::memory_queries.push_back(device);
    if (device == fake_hip::fail_memory_on) return hipErrorUnknown;
    *free = fake_hip::devices.at(device).free;
    *total = fake_hip::devices.at(device).total;
    return hipSuccess;
}
inline hipError_t hipDeviceGetPCIBusId(char* bdf, int capacity, int device) {
    std::snprintf(bdf, capacity, "%s", fake_hip::devices.at(device).bdf.c_str());
    return hipSuccess;
}
inline hipError_t hipDeviceGetUuid(hipUUID* uuid, int device) {
    *uuid = fake_hip::devices.at(device).uuid;
    return hipSuccess;
}
