// Compile the production discovery implementation against a private API double,
// so CPU CI covers partition layouts without installing HIP or repartitioning.
#include "../../src/backend/hip/discovery.hip"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;
using namespace keyhunt::backend;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct TemporarySysfs {
    fs::path path;
    TemporarySysfs() {
        char pattern[] = "/tmp/keyhunt-discovery-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~TemporarySysfs() { fs::remove_all(path); }
};
void attribute(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << value << '\n';
}
void check_inventory(const fs::path& root) {
    fake_hip::memory_queries.clear();
    const int previous = fake_hip::selected;
    const auto result = discover_hip_at(root);
    require(result.devices.size() == fake_hip::devices.size(), "device count was inferred from partition mode");
    require(fake_hip::selected == previous, "discovery changed the caller's selected device");
    for (size_t i = 0; i < result.devices.size(); ++i) {
        const auto& actual = result.devices[i];
        const auto& expected = fake_hip::devices[i];
        require(actual.ordinal == int(i) && fake_hip::memory_queries.at(i) == int(i), "wrong device memory queried");
        require(actual.pci_bus_id == expected.bdf, "identity lost after visibility remapping");
        require(actual.property_memory_bytes == expected.properties.totalGlobalMem &&
                actual.total_memory_bytes == expected.total && actual.free_memory_bytes == expected.free,
                "memory was scaled or cached instead of queried for each logical device");
        require(actual.compute_units == expected.properties.multiProcessorCount, "CU count was hard-coded");
        const auto path = root / actual.pci_bus_id;
        if (fs::exists(path)) {
            require(actual.compute_partition == read_attribute(path / "current_compute_partition"), "compute mode changed");
            require(actual.memory_partition == read_attribute(path / "current_memory_partition"), "NPS mode inferred from compute mode");
            require(!actual.physical_id.empty() && actual.warnings.empty(), "valid metadata lost");
        } else {
            require(actual.physical_id.empty() && actual.compute_partition.empty() && actual.memory_partition.empty(),
                    "synthetic partition BDF was assigned a guessed parent");
            require(actual.warnings.size() == 2, "missing metadata was not reported");
        }
    }
}
void profile(const char* compute, const char* memory, int partitions) {
    TemporarySysfs sysfs;
    fake_hip::devices.clear();
    // Three synthetic packages avoid baking this particular eight-card host into
    // discovery. Only the primary BDF has sysfs metadata, as on the CPX host.
    for (int i = 0; i < 3 * partitions; ++i) {
        fake_hip::Device device;
        std::snprintf(device.properties.name, sizeof(device.properties.name), "fixture MI300X");
        std::snprintf(device.properties.gcnArchName, sizeof(device.properties.gcnArchName), "gfx942");
        device.properties.multiProcessorCount = 304 / partitions;
        device.properties.warpSize = 64;
        device.properties.totalGlobalMem = (192ULL << 30) / partitions;
        // Runtime limits intentionally differ from the product HBM size and
        // from each other, catching attempts to divide package memory by mode.
        device.total = device.properties.totalGlobalMem - 1048576 * (i + 1);
        device.free = device.total - 4096 * (i + 1);
        device.uuid.bytes[0] = static_cast<char>(i + 1);
        char bdf[32];
        std::snprintf(bdf, sizeof(bdf), "0000:%02x:00.%d", 0x20 + i / partitions, i % partitions);
        device.bdf = bdf;
        if (i % partitions == 0) {
            const auto path = sysfs.path / device.bdf;
            attribute(path / "unique_id", "fixture-package-" + std::to_string(i / partitions));
            attribute(path / "current_compute_partition", compute);
            attribute(path / "current_memory_partition", memory);
            attribute(path / "compute_partition_mem_alloc_mode", "CAPPING");
            attribute(path / "numa_node", "0");
        }
        fake_hip::devices.push_back(device);
    }
    fake_hip::selected = int(fake_hip::devices.size()) - 1;
    check_inventory(sysfs.path);
    // Renumber a restricted/reordered set, then rediscover without cached state.
    fake_hip::devices = {fake_hip::devices.back(), fake_hip::devices.front()};
    fake_hip::selected = 1;
    check_inventory(sysfs.path);
    fake_hip::fail_memory_on = 0;
    fake_hip::memory_queries.clear();
    fake_hip::selected=0;
    const auto selected=select_hip_at(sysfs.path,1);
    require(selected.visible_devices==2 && selected.device.ordinal==1 &&
        selected.device.pci_bus_id==fake_hip::devices[1].bdf,"selected device identity/remapping");
    require(fake_hip::memory_queries==std::vector<int>{1},"selected query visited unrelated failing device");
    require(fake_hip::selected==1,"selected query did not leave owner on its chosen device");
    try {
        (void)discover_hip_at(sysfs.path);
        throw std::logic_error("memory query failure was ignored");
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find("hipMemGetInfo") != std::string::npos, "wrong failure diagnostic");
        require(fake_hip::selected == 1, "selection not restored after query failure");
    }
    fake_hip::fail_memory_on = -1;
    fake_hip::devices.clear();
    require(discover_hip_at(sysfs.path).devices.empty(), "hidden devices were resurrected");
    std::cout << compute << '/' << memory << ": discovery, budgets, remapping and failures passed\n";
}
int main() {
    try {
        profile("SPX", "NPS1", 1);
        profile("QPX", "NPS1", 4);
        profile("QPX", "NPS4", 4);
        profile("CPX", "NPS1", 8);
        profile("CPX", "NPS4", 8);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
