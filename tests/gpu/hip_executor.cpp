#include "keyhunt/backend/device.h"
#include "keyhunt/backend/hip_executor.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace keyhunt;
using core::UInt256;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F fn, const char* message) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
scheduler::KernelBatch plan(UInt256 begin, uint64_t count) {
    scheduler::ExecutionIdentity identity;
    identity.job_digest[0] = 11; identity.target_digest[31] = 22;
    identity.algorithm_digest[7] = 33; identity.assignment_id[5] = 44;
    identity.assignment_generation = 5; identity.executor_generation = 6;
    // Include a nonzero parent block and a cursor inside it, to check identity
    // and exact bounds survive the asynchronous round trip unchanged.
    const auto root = begin.subtract(UInt256(count + 2));
    scheduler::BlockGrid grid(core::ScalarInterval(root, begin.add(UInt256(count))), UInt256(count + 1));
    const auto work = scheduler::WorkUnit::plan(grid, UInt256(1), begin, count, identity);
    return *scheduler::KernelBatch::plan(*work, begin, count);
}
int main() {
    try {
        require(!backend::discover_hip().devices.empty(), "HIP hardware required; no device visible");
        rejects([] { backend::HipDiagnosticExecutor e(-1); }, "invalid device accepted");
        rejects([] { backend::HipDiagnosticExecutor e(0, {0}); }, "zero capacity accepted");
        rejects([] { backend::HipDiagnosticExecutor e(0, {1048577}); }, "unbounded capacity accepted");
        rejects([] { backend::HipDiagnosticExecutor e(0, {1, std::numeric_limits<uint64_t>::max()}); }, "memory headroom ignored");
        backend::HipDiagnosticExecutor executor(0, {1024}), other(0, {1024});
        executor.drain();
        auto begin = UInt256::from_hex("100000000ffffffffffffffff");
        const auto first = plan(begin, 257);
        const auto ticket = executor.submit(first);
        rejects([&] { executor.submit(first); }, "busy slot overwritten");
        const auto other_ticket = other.submit(first);
        rejects([&] { other.poll(ticket); }, "foreign ticket accepted");
        other.drain();
        require(other.take(other_ticket).device_steps == 257, "independent stream failed");
        executor.drain();
        rejects([&] { executor.submit(first); }, "drain discarded an unconsumed result");
        auto kept = executor.take(ticket);
        rejects([&] { executor.take(ticket); }, "result consumed twice");
        require(kept.batch.work().identity() == first.work().identity(), "identity changed");
        require(kept.batch.work().block_id() == first.work().block_id(), "block ID changed");
        uint64_t launches = 1, steps = kept.device_steps;
        for (uint64_t count : {1, 255, 256, 257, 511, 1023, 1024}) {
            auto batch = plan(begin, count);
            auto next = executor.submit(batch);
            rejects([&] { executor.poll(ticket); }, "stale ticket accepted after slot reuse");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!executor.poll(next)) {
                require(std::chrono::steady_clock::now() < deadline, "HIP completion timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto result = executor.take(next);
            require(result.device_steps == count && result.scalars.size() == count, "partial batch count wrong");
            require(result.scalars.front() == begin && result.scalars.back() == begin.add(UInt256(count - 1)), "scalar bounds wrong");
            require(result.batch.interval().end() == batch.interval().end(), "batch endpoint changed");
            require(result.kernel_ms >= 0 && result.download_ms >= 0 && result.wall_ms >= result.verification_ms, "invalid timing");
            launches += result.launch_count; steps += result.device_steps;
        }
        const auto high = plan(core::scalar_order().subtract(UInt256(2)), 2);
        auto last = executor.submit(high); executor.drain();
        auto result = executor.take(last);
        require(result.scalars.back() == core::scalar_order().subtract(UInt256(1)), "order boundary wrong");
        require(kept.scalars.front() == begin, "owned result changed after slot reuse");
        backend::HipDiagnosticExecutor small(0, {1});
        rejects([&] { small.submit(first); }, "oversized batch accepted");
        // Destruction with queued work must drain before freeing pinned memory.
        { backend::HipDiagnosticExecutor pending(0); pending.submit(first); }
        std::cout << "HIP lifecycle passed: " << launches + 1 << " verified launches, "
                  << steps + result.device_steps << " device indices\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
