#include "keyhunt/backend/device.h"
#include <stdexcept>

namespace keyhunt::backend {
namespace {
[[noreturn]] void unavailable(const char* backend, const char* option) {
    throw std::runtime_error(std::string(backend) + " backend is not built; configure with " + option + "=ON");
}
}
bool hip_available() {
#ifdef KEYHUNT_HAS_HIP
    return true;
#else
    return false;
#endif
}
bool cuda_available() {
#ifdef KEYHUNT_HAS_CUDA
    return true;
#else
    return false;
#endif
}
#ifndef KEYHUNT_HAS_HIP
DeviceInventory discover_hip() { unavailable("HIP", "KEYHUNT_ENABLE_HIP"); }
#endif
#ifndef KEYHUNT_HAS_CUDA
DeviceInventory discover_cuda() { unavailable("CUDA", "KEYHUNT_ENABLE_CUDA"); }
#endif
const char* gpu_backend_name() {
    return cuda_available() ? "cuda" : hip_available() ? "hip" : "none";
}
void require_backend(const std::string& name) {
    if (name == "cuda") {
        if (!cuda_available()) unavailable("CUDA", "KEYHUNT_ENABLE_CUDA");
    } else if (name == "hip") {
        if (!hip_available()) unavailable("HIP", "KEYHUNT_ENABLE_HIP");
    } else throw std::invalid_argument("usage: --backend hip|cuda");
}
DeviceInventory discover_gpu() {
    if (cuda_available()) return discover_cuda();
    return discover_hip();
}
SelectedDevice select_gpu(int ordinal) {
#ifdef KEYHUNT_HAS_CUDA
    SelectedDevice select_cuda(int);
    return select_cuda(ordinal);
#else
    auto inventory = discover_hip();
    if (ordinal < 0 || size_t(ordinal) >= inventory.devices.size())
        throw std::invalid_argument("GPU device ordinal is not visible");
    return {std::move(inventory.devices[size_t(ordinal)]), inventory.devices.size()};
#endif
}
}
