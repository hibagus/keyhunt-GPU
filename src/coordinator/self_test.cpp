#include "self_test.h"
#include "keyhunt/backend/device.h"
#include "keyhunt/storage/checkpoint.h"
#include <set>
namespace keyhunt::coordination {
Json device_self_test(int ordinal){
#ifndef KEYHUNT_HAS_GPU
    (void)ordinal;throw std::runtime_error("worker self-test requires a GPU build; there is no CPU execution fallback");
#else
    using namespace keyhunt;using namespace core;
    // Device enumeration may initialize every visible runtime context. A
    // per-device self-test must query only the device whose owner lock is held.
    const auto selected=backend::select_gpu(ordinal);
    const auto& device=selected.device;
    XPointVerifier verifier;std::vector<XPointBytes> xs;std::vector<UncompressedPublicKey> points;
    for(uint64_t scalar:{1,17,32,40}){
        const auto point=verifier.derive(UInt256(scalar));points.push_back(point);
        XPointBytes x;std::copy_n(point.begin()+1,32,x.begin());xs.push_back(x);
    }
    const XPointTargets targets(xs);const ScalarInterval interval(UInt256(1),UInt256(34));
    scheduler::ExecutionIdentity identity;identity.target_digest=targets.digest();identity.assignment_id[0]=1;identity.assignment_generation=1;identity.executor_generation=1;
    const scheduler::BlockGrid grid(interval,UInt256(33));
    const auto work=scheduler::WorkUnit::plan(grid,UInt256(),UInt256(1),33,identity);
    const auto batch=scheduler::KernelBatch::plan(*work,UInt256(1),33);
    const std::set<UInt256> expected{UInt256(1),UInt256(17),UInt256(32)};
    // Hit, miss, boundary and tail vectors exercise field/point/search paths.
    // Run both xpoint variants and both BSGS groups on the visible partition.
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped}){
        backend::XPointOptions options;options.max_steps=64;options.kernel=kernel;options.candidate_capacity=16;
        backend::GpuXPointExecutor gpu(ordinal,targets,verifier,options);
        const auto ticket=gpu.submit(*batch);gpu.drain();const auto result=gpu.take(ticket);
        std::set<UInt256> found;for(const auto& match:result.matches)found.insert(match.scalar);
        if(result.overflow||result.verified_steps!=33||found!=expected||result.matches.size()!=expected.size())
            throw std::runtime_error("GPU xpoint runtime self-test failed");
    }
    const auto table=bsgs::Table::build(16);const BsgsPublicKeyTargets btargets(points);
    const BsgsBatch bb(interval,16,0,4,btargets.digest(),table.checksum());
    for(unsigned group:{1U,8U}){
        backend::BsgsSearchOptions options;options.max_steps=64;options.group_size=group;options.candidate_capacity=16;
        backend::GpuBsgsExecutor gpu(ordinal,table,btargets,verifier,options);
        const auto ticket=gpu.submit(bb);gpu.drain();const auto result=gpu.take(ticket);
        std::set<UInt256> found;for(const auto& match:result.matches)found.insert(match.scalar);
        if(result.overflow||result.verified_steps!=bb.steps()||found!=expected||result.matches.size()!=expected.size())
            throw std::runtime_error("GPU BSGS runtime self-test failed");
    }
    return {{"passed",true},{"ordinal",ordinal},{"uuid",device.uuid},{"pci_bus_id",device.pci_bus_id},
        {"compute_units",device.compute_units},{"compute_partition",device.compute_partition},
        {"memory_partition",device.memory_partition},{"runtime",selected.runtime_version},{"driver",selected.driver_version}};
#endif
}
}
