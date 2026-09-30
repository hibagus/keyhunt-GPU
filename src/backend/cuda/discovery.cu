#include "keyhunt/backend/device.h"
#include "runtime.h"
#include <fstream>
#include <iomanip>
#include <sstream>

namespace keyhunt::backend {
DeviceInventory discover_cuda() {
    DeviceInventory result;
    gpu_check(cudaRuntimeGetVersion(&result.runtime_version), "cudaRuntimeGetVersion");
    gpu_check(cudaDriverGetVersion(&result.driver_version), "cudaDriverGetVersion");
    int count = 0;
    const auto status = cudaGetDeviceCount(&count);
    if (status == cudaErrorNoDevice) return result;
    gpu_check(status, "cudaGetDeviceCount");
    for (int ordinal = 0; ordinal < count; ++ordinal) {
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
        std::ostringstream hex;
        for (unsigned char byte : prop.uuid.bytes)
            hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        info.uuid = hex.str();
        // Runtime UUIDs identify visible instances, including MIG. Neither a
        // UUID nor a shared PCI BDF proves physical-package identity under MIG.
        info.warnings.push_back("physical package identity unavailable");
        info.warnings.push_back("partition metadata unavailable");
        std::ifstream("/sys/bus/pci/devices/" + info.pci_bus_id + "/numa_node") >> info.numa_node;
        result.devices.push_back(std::move(info));
    }
    return result;
}
}
