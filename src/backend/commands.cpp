#include "keyhunt/backend/commands.h"
#include "keyhunt/backend/device.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
namespace {
// Escape runtime-provided names as JSON, including control characters.
std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
void print_inventory(const DeviceInventory& inventory) {
    std::cout << "{\"backend\":\"hip\",\"runtime_version\":" << inventory.runtime_version
              << ",\"driver_version\":" << inventory.driver_version << ",\"devices\":[";
    bool first = true;
    for (const auto& d : inventory.devices) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"ordinal\":" << d.ordinal;
        const auto field = [](const char* key, const std::string& value) {
            std::cout << ',' << quoted(key) << ':' << quoted(value);
        };
        field("name", d.name); field("architecture", d.architecture);
        field("uuid", d.uuid); field("pci_bus_id", d.pci_bus_id);
        field("physical_id", d.physical_id); field("physical_id_source", d.physical_id_source);
        field("compute_partition", d.compute_partition); field("memory_partition", d.memory_partition);
        field("memory_allocation_mode", d.memory_allocation_mode);
        std::cout << ",\"numa_node\":" << d.numa_node << ",\"compute_units\":" << d.compute_units
                  << ",\"warp_size\":" << d.warp_size
                  << ",\"property_memory_bytes\":" << d.property_memory_bytes
                  << ",\"total_memory_bytes\":" << d.total_memory_bytes
                  << ",\"free_memory_bytes\":" << d.free_memory_bytes << ",\"warnings\":[";
        for (size_t i = 0; i < d.warnings.size(); ++i)
            std::cout << (i ? "," : "") << quoted(d.warnings[i]);
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
}
int dispatch_command(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "devices") return -1;
    try {
        if (argc != 4 || std::string(argv[2]) != "--backend" || std::string(argv[3]) != "hip")
            throw std::invalid_argument("usage: keyhunt devices --backend hip (JSON output)");
        print_inventory(discover_hip());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "keyhunt: " << error.what() << '\n';
        return 2;
    }
}
}
