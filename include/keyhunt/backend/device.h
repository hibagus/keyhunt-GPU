#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace keyhunt::backend {

// Ordinals are process-local (visibility variables can renumber them). Persist
// the reported identity instead; a logical partition is not a physical card.
struct DeviceInfo {
    int ordinal = 0;
    std::string name, architecture, uuid, pci_bus_id;
    std::string physical_id, physical_id_source;
    // Optional observed metadata, never a mode whitelist or memory multiplier.
    // Missing sysfs metadata on a CPX/QPX sibling does not disable execution.
    std::string compute_partition, memory_partition, memory_allocation_mode;
    int numa_node = -1;
    int compute_units = 0, warp_size = 0;
    uint64_t property_memory_bytes = 0, total_memory_bytes = 0, free_memory_bytes = 0;
    std::vector<std::string> warnings;
};
struct DeviceInventory {
    int runtime_version = 0, driver_version = 0;
    std::vector<DeviceInfo> devices;
};

bool hip_available();
DeviceInventory discover_hip(); // throws an operation-specific error on runtime failure
bool cuda_available();
DeviceInventory discover_cuda();

// A binary supplies one native GPU runtime. Explicit CLI selection must match
// it; persisted job identity and table formats never depend on this selection.
const char* gpu_backend_name();
void require_backend(const std::string& name);
DeviceInventory discover_gpu();

} // namespace keyhunt::backend
