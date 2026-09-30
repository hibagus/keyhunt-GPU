#pragma once
// Private, zero-overhead SDK spelling adapter. Kernels launch through the native
// compiler; stream ownership and error handling live in gpu/runtime.h.
#if defined(__CUDACC__)
#include <cuda_runtime.h>
#else
#include <hip/hip_runtime.h>
#endif

#if defined(__CUDACC__)
using gpuStream_t = cudaStream_t;
using gpuEvent_t = cudaEvent_t;
using gpuError_t = cudaError_t;
inline constexpr auto gpuSuccess = cudaSuccess;
inline constexpr auto gpuErrorNotReady = cudaErrorNotReady;
inline constexpr auto gpuMemcpyHostToDevice = cudaMemcpyHostToDevice;
inline constexpr auto gpuMemcpyDeviceToHost = cudaMemcpyDeviceToHost;
inline constexpr auto gpuStreamNonBlocking = cudaStreamNonBlocking;
inline constexpr auto gpuDeviceAttributeMultiprocessorCount = cudaDevAttrMultiProcessorCount;
#define KEYHUNT_NATIVE_API(name) cuda##name
#else
using gpuStream_t = hipStream_t;
using gpuEvent_t = hipEvent_t;
using gpuError_t = hipError_t;
inline constexpr auto gpuSuccess = hipSuccess;
inline constexpr auto gpuErrorNotReady = hipErrorNotReady;
inline constexpr auto gpuMemcpyHostToDevice = hipMemcpyHostToDevice;
inline constexpr auto gpuMemcpyDeviceToHost = hipMemcpyDeviceToHost;
inline constexpr auto gpuStreamNonBlocking = hipStreamNonBlocking;
inline constexpr auto gpuDeviceAttributeMultiprocessorCount = hipDeviceAttributeMultiprocessorCount;
#define KEYHUNT_NATIVE_API(name) hip##name
#endif
#define gpuLaunchKernelGGL(kernel, grid, block, shared, stream, ...) \
    kernel<<<grid, block, shared, stream>>>(__VA_ARGS__)

inline gpuError_t gpuGetDevice(int* value) { return KEYHUNT_NATIVE_API(GetDevice)(value); }
inline gpuError_t gpuGetDeviceCount(int* value) { return KEYHUNT_NATIVE_API(GetDeviceCount)(value); }
inline gpuError_t gpuSetDevice(int value) { return KEYHUNT_NATIVE_API(SetDevice)(value); }
inline gpuError_t gpuMemGetInfo(size_t* free, size_t* total) { return KEYHUNT_NATIVE_API(MemGetInfo)(free, total); }
inline gpuError_t gpuStreamCreateWithFlags(gpuStream_t* value, unsigned flags) { return KEYHUNT_NATIVE_API(StreamCreateWithFlags)(value, flags); }
inline gpuError_t gpuStreamDestroy(gpuStream_t value) { return KEYHUNT_NATIVE_API(StreamDestroy)(value); }
inline gpuError_t gpuStreamSynchronize(gpuStream_t value) { return KEYHUNT_NATIVE_API(StreamSynchronize)(value); }
inline gpuError_t gpuEventCreate(gpuEvent_t* value) { return KEYHUNT_NATIVE_API(EventCreate)(value); }
inline gpuError_t gpuEventDestroy(gpuEvent_t value) { return KEYHUNT_NATIVE_API(EventDestroy)(value); }
inline gpuError_t gpuEventRecord(gpuEvent_t value, gpuStream_t stream = nullptr) { return KEYHUNT_NATIVE_API(EventRecord)(value, stream); }
inline gpuError_t gpuEventQuery(gpuEvent_t value) { return KEYHUNT_NATIVE_API(EventQuery)(value); }
inline gpuError_t gpuEventSynchronize(gpuEvent_t value) { return KEYHUNT_NATIVE_API(EventSynchronize)(value); }
inline gpuError_t gpuEventElapsedTime(float* ms, gpuEvent_t start, gpuEvent_t end) { return KEYHUNT_NATIVE_API(EventElapsedTime)(ms, start, end); }
inline gpuError_t gpuFree(void* ptr) { return KEYHUNT_NATIVE_API(Free)(ptr); }
inline gpuError_t gpuMemset(void* ptr, int value, size_t bytes) { return KEYHUNT_NATIVE_API(Memset)(ptr, value, bytes); }
inline gpuError_t gpuMemsetAsync(void* ptr, int value, size_t bytes, gpuStream_t stream) { return KEYHUNT_NATIVE_API(MemsetAsync)(ptr, value, bytes, stream); }
inline gpuError_t gpuGetLastError() { return KEYHUNT_NATIVE_API(GetLastError)(); }
template<class T> inline gpuError_t gpuMalloc(T** ptr, size_t bytes) {
    return KEYHUNT_NATIVE_API(Malloc)(reinterpret_cast<void**>(ptr), bytes);
}
template<class Kind> inline gpuError_t gpuMemcpy(void* dst, const void* src, size_t bytes, Kind kind) {
    return KEYHUNT_NATIVE_API(Memcpy)(dst, src, bytes, kind);
}
template<class Kind> inline gpuError_t gpuMemcpyAsync(void* dst, const void* src, size_t bytes, Kind kind, gpuStream_t stream) {
    return KEYHUNT_NATIVE_API(MemcpyAsync)(dst, src, bytes, kind, stream);
}
template<class Attribute> inline gpuError_t gpuDeviceGetAttribute(int* value, Attribute attribute, int device) {
    return KEYHUNT_NATIVE_API(DeviceGetAttribute)(value, attribute, device);
}
inline const char* gpuGetErrorName(gpuError_t status) { return KEYHUNT_NATIVE_API(GetErrorName)(status); }
inline const char* gpuGetErrorString(gpuError_t status) { return KEYHUNT_NATIVE_API(GetErrorString)(status); }
template<class T> inline gpuError_t gpuHostMalloc(T** ptr, size_t bytes) {
#if defined(__CUDACC__)
    return cudaMallocHost(reinterpret_cast<void**>(ptr), bytes);
#else
    return hipHostMalloc(reinterpret_cast<void**>(ptr), bytes);
#endif
}
inline gpuError_t gpuHostFree(void* ptr) {
#if defined(__CUDACC__)
    return cudaFreeHost(ptr);
#else
    return hipHostFree(ptr);
#endif
}
#undef KEYHUNT_NATIVE_API
