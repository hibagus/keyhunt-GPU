#include "keyhunt/backend/device.h"
#include "runtime.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace keyhunt::backend {
namespace {
int visible_count() {
    int count = 0;
    const auto status = cudaGetDeviceCount(&count);
    if (status == cudaErrorNoDevice) return 0;
    gpu_check(status, "cudaGetDeviceCount");
    return count;
}
DeviceInfo describe(int ordinal) {
    DeviceScope selected(ordinal);
    cudaDeviceProp prop{};
    gpu_check(cudaGetDeviceProperties(&prop, ordinal), "cudaGetDeviceProperties");
    DeviceInfo info;
    info.ordinal = ordinal;
    info.name = prop.name;
    info.architecture = "sm_" + std::to_string(prop.major) + std::to_string(prop.minor);
    info.compute_units = prop.multiProcessorCount;
    info.warp_size = prop.warpSize;
    info.property_memory_bytes = prop.totalGlobalMem;
    size_t free = 0, total = 0;
    gpu_check(cudaMemGetInfo(&free, &total), "cudaMemGetInfo");
    info.free_memory_bytes = free;
    info.total_memory_bytes = total;
    char bdf[32]{};
    gpu_check(cudaDeviceGetPCIBusId(bdf, sizeof(bdf), ordinal), "cudaDeviceGetPCIBusId");
    info.pci_bus_id = bdf;
    // CUDA may use uppercase hex; Linux PCI sysfs directory names are lowercase.
    std::transform(info.pci_bus_id.begin(), info.pci_bus_id.end(), info.pci_bus_id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::ostringstream hex;
    for (unsigned char byte : prop.uuid.bytes)
        hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    info.uuid = hex.str();
    // Runtime UUIDs identify visible instances, including MIG. Neither a
    // UUID nor a shared PCI BDF proves physical-package identity under MIG.
    info.warnings.push_back("physical package identity unavailable");
    info.warnings.push_back("partition metadata unavailable");
    std::ifstream("/sys/bus/pci/devices/" + info.pci_bus_id + "/numa_node") >> info.numa_node;
    return info;
}
}
DeviceInventory discover_cuda() {
    DeviceInventory result;
    gpu_check(cudaRuntimeGetVersion(&result.runtime_version), "cudaRuntimeGetVersion");
    gpu_check(cudaDriverGetVersion(&result.driver_version), "cudaDriverGetVersion");
    const int count = visible_count();
    for (int ordinal = 0; ordinal < count; ++ordinal)
        result.devices.push_back(describe(ordinal));
    return result;
}
SelectedDevice select_cuda(int ordinal) {
    const int count = visible_count();
    if (ordinal < 0 || ordinal >= count)
        throw std::invalid_argument("GPU device ordinal is not visible");
    return {describe(ordinal), size_t(count)};
}
}
