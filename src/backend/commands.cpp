#include "keyhunt/backend/commands.h"
#include "keyhunt/backend/device.h"

#ifdef KEYHUNT_HAS_HIP
#include "keyhunt/backend/hip_executor.h"
#endif
#include <charconv>
#include <chrono>
#include <thread>
#include <limits>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace keyhunt::backend {
int xpoint_command(int argc, char** argv);
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
uint64_t number(const std::string& value) {
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("expected an unsigned decimal integer: " + value);
    return result;
}
int smoke(int argc, char** argv) {
    const char* usage = "usage: keyhunt gpu-smoke --backend hip [--device N] [--steps 1..1048576] [--start HEX]";
    int device = 0;
    uint64_t steps = 257;
    std::string start = "0x100000000ffffffffffffffff";
    bool backend = false, seen_device = false, seen_steps = false, seen_start = false;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) throw std::invalid_argument(usage);
        const std::string key = argv[i], value = argv[i + 1];
        if (key == "--backend" && !backend && value == "hip") backend = true;
        else if (key == "--device" && !seen_device) {
            const auto parsed = number(value);
            if (parsed > std::numeric_limits<int>::max()) throw std::invalid_argument("device ordinal is too large");
            device = static_cast<int>(parsed); seen_device = true;
        } else if (key == "--steps" && !seen_steps) { steps = number(value); seen_steps = true; }
        else if (key == "--start" && !seen_start) { start = value; seen_start = true; }
        else throw std::invalid_argument(usage);
    }
    if (!backend) throw std::invalid_argument(usage);
    if (steps == 0 || steps > 1048576) throw std::invalid_argument("steps must be in [1, 1048576]");
#ifndef KEYHUNT_HAS_HIP
    (void)device;
    discover_hip(); // same explicit unavailable-backend error as discovery
    return 2;
#else
    using core::UInt256;
    const auto begin = UInt256::from_hex(start);
    const core::ScalarInterval interval(begin, begin.add(UInt256(steps)));
    scheduler::BlockGrid grid(interval, UInt256(steps));
    scheduler::ExecutionIdentity identity;
    identity.assignment_id[0] = 1;
    identity.assignment_generation = identity.executor_generation = 1;
    const auto work = scheduler::WorkUnit::plan(grid, UInt256(0), begin, steps, identity);
    const auto batch = scheduler::KernelBatch::plan(*work, begin, steps);
    const auto inventory = discover_hip();
    if (size_t(device) >= inventory.devices.size()) throw std::invalid_argument("HIP device ordinal is not visible");
    HipDiagnosticExecutor executor(device, ExecutorOptions{steps});
    const auto ticket = executor.submit(*batch);
    while (!executor.poll(ticket)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto result = executor.take(ticket);
    std::cout << "{\"backend\":\"hip\",\"diagnostic_only\":true,\"search_coverage\":false,\"device\":" << device
              << ",\"uuid\":" << quoted(inventory.devices[device].uuid)
              << ",\"architecture\":" << quoted(inventory.devices[device].architecture)
              << ",\"begin\":" << quoted(interval.begin().hex()) << ",\"end_exclusive\":" << quoted(interval.end().hex())
              << ",\"device_steps\":" << result.device_steps << ",\"launch_count\":" << result.launch_count
              << ",\"device_allocation_bytes\":" << result.device_allocation_bytes
              << ",\"pinned_allocation_bytes\":" << result.pinned_allocation_bytes
              << ",\"download_bytes\":" << result.download_bytes
              << ",\"kernel_ms\":" << result.kernel_ms << ",\"download_ms\":" << result.download_ms
              << ",\"verification_ms\":" << result.verification_ms << ",\"wall_ms\":" << result.wall_ms << "}\n";
    return 0;
#endif
}
}
int dispatch_command(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string command = argv[1];
    if (command != "devices" && command != "gpu-smoke" && command != "xpoint") return -1;
    try {
        if (command == "gpu-smoke") return smoke(argc, argv);
        if (command == "xpoint") return xpoint_command(argc, argv);
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
