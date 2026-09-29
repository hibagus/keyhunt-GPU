#include "keyhunt/backend/device.h"
#include "runtime.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace keyhunt::backend {
namespace {
std::string read_attribute(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return value;
}
void topology(DeviceInfo& info, const std::filesystem::path& sysfs_root) {
    const auto path = sysfs_root / info.pci_bus_id;
    // Some drivers expose a partition BDF without a matching PCI sysfs node.
    // Do not guess a parent by rounding that BDF or invent package counts.
    info.physical_id = read_attribute(path / "unique_id");
    if (!info.physical_id.empty() && info.physical_id != "0")
        info.physical_id_source = "sysfs.unique_id";
    else {
        info.physical_id.clear();
        info.warnings.push_back("physical package identity unavailable");
    }
    info.compute_partition = read_attribute(path / "current_compute_partition");
    info.memory_partition = read_attribute(path / "current_memory_partition");
    info.memory_allocation_mode = read_attribute(path / "compute_partition_mem_alloc_mode");
    if (info.compute_partition.empty() || info.memory_partition.empty())
        info.warnings.push_back("partition metadata unavailable");
    std::istringstream(read_attribute(path / "numa_node")) >> info.numa_node;
}
}

bool hip_available() { return true; }
// The root is an internal test seam, not a user-controlled device selector.
// HIP ordinals and properties stay authoritative in CPX, QPX and SPX alike.
static DeviceInventory discover_hip_at(const std::filesystem::path& sysfs_root) {
    DeviceInventory result;
    hip_check(hipRuntimeGetVersion(&result.runtime_version), "hipRuntimeGetVersion");
    hip_check(hipDriverGetVersion(&result.driver_version), "hipDriverGetVersion");
    int count = 0;
    const auto status = hipGetDeviceCount(&count);
    if (status == hipErrorNoDevice) return result;
    hip_check(status, "hipGetDeviceCount");
    for (int i = 0; i < count; ++i) {
        DeviceScope selected(i);
        hipDeviceProp_t prop{};
        hip_check(hipGetDeviceProperties(&prop, i), "hipGetDeviceProperties");
        DeviceInfo info;
        info.ordinal = i;
        info.name = prop.name;
        info.architecture = prop.gcnArchName;
        info.compute_units = prop.multiProcessorCount;
        info.warp_size = prop.warpSize;
        info.property_memory_bytes = prop.totalGlobalMem;
        size_t free = 0, total = 0;
        hip_check(hipMemGetInfo(&free, &total), "hipMemGetInfo");
        info.free_memory_bytes = free;
        info.total_memory_bytes = total;
        char bdf[32]{};
        hip_check(hipDeviceGetPCIBusId(bdf, sizeof(bdf), i), "hipDeviceGetPCIBusId");
        info.pci_bus_id = bdf;
        hipUUID uuid{};
        hip_check(hipDeviceGetUuid(&uuid, i), "hipDeviceGetUuid");
        std::ostringstream hex;
        for (unsigned char byte : uuid.bytes)
            hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        info.uuid = hex.str();
        topology(info, sysfs_root);
        result.devices.push_back(std::move(info));
    }
    return result;
}
DeviceInventory discover_hip() {
    return discover_hip_at("/sys/bus/pci/devices");
}
}
