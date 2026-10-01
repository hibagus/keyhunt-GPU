#include "self_test.h"
#include "keyhunt/backend/device.h"
#include "keyhunt/storage/checkpoint.h"
#include <set>
namespace keyhunt::coordination {
#ifdef KEYHUNT_HAS_GPU
namespace {
template<class Executor,class Targets,class Relation>
void stride_self_test(int ordinal,const Targets& targets,scheduler::WorkAlgorithm family,
                      const core::XPointVerifier& verifier,Relation relation){
    using core::UInt256;
    for(bool orbit:{false,true})for(bool reverse:{false,true}){
        const core::ScalarStride mapping({UInt256(1),UInt256(34)},UInt256(8),reverse,orbit);
        scheduler::ExecutionIdentity identity;identity.algorithm=scheduler::strided_algorithm(family,reverse,orbit);
        identity.stride_mapping=mapping;identity.target_digest=targets.digest();identity.assignment_id[0]=1;
        identity.assignment_generation=identity.executor_generation=1;
        const scheduler::BlockGrid grid(mapping.indices(),mapping.indices().size());
        for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped,backend::XPointKernel::Glv}){
            backend::XPointOptions options;options.stride=UInt256(8);options.reverse=reverse;options.orbit=orbit;
            options.max_steps=5;options.candidate_capacity=128;options.kernel=kernel;
            Executor gpu(ordinal,targets,verifier,options);
            for(unsigned variant=0;variant<(orbit?6U:1U);++variant){
                const auto begin=UInt256(1+5*variant);
                const auto work=*scheduler::WorkUnit::plan(grid,UInt256(),begin,5,identity);
                const auto batch=*scheduler::KernelBatch::plan(work,begin,5);
                std::set<std::pair<UInt256,uint32_t>> expected;
                for(unsigned i=0;i<5;++i){
                    const auto seed=UInt256(1+8*(reverse?4-i:i));
                    const auto pub=verifier.derive(core::scalar_orbit(seed,variant));
                    for(uint32_t t=0;t<targets.values().size();++t)
                        if(relation(pub,targets.values()[t]))expected.emplace(begin.add(UInt256(i)),t);
                }
                const auto ticket=gpu.submit(batch);gpu.drain();const auto result=gpu.take(ticket);
                std::set<std::pair<UInt256,uint32_t>> found;for(const auto& match:result.matches)found.emplace(match.scalar,match.target);
                if(result.overflow||result.verified_steps!=5||found!=expected||result.matches.size()!=expected.size())
                    throw std::runtime_error("GPU scalar mapping runtime self-test failed");
            }
        }
    }
}
}
#endif
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
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped,backend::XPointKernel::Glv}){
        backend::XPointOptions options;options.max_steps=64;options.kernel=kernel;options.candidate_capacity=16;
        backend::GpuXPointExecutor gpu(ordinal,targets,verifier,options);
        const auto ticket=gpu.submit(*batch);gpu.drain();const auto result=gpu.take(ticket);
        std::set<UInt256> found;for(const auto& match:result.matches)found.insert(match.scalar);
        if(result.overflow||result.verified_steps!=33||found!=expected||result.matches.size()!=expected.size())
            throw std::runtime_error("GPU xpoint runtime self-test failed");
    }
    std::vector<Hash160Target> hashes;
    for(const auto& point:points)for(uint8_t tag:{1,2})hashes.push_back(hash160_target(point,tag));
    const Hash160Targets htargets(std::move(hashes));
    identity.algorithm=scheduler::WorkAlgorithm::DirectHash160V1;identity.target_digest=htargets.digest();
    const auto hwork=*scheduler::WorkUnit::plan(grid,UInt256(),UInt256(1),33,identity);
    const auto hbatch=*scheduler::KernelBatch::plan(hwork,UInt256(1),33);
    // Exercise both serializations and kernel variants in the new process. A
    // persisted self-test result cannot bypass a changed runtime or binary.
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped,backend::XPointKernel::Glv}){
        backend::Hash160Options options;options.max_steps=64;options.kernel=kernel;options.candidate_capacity=16;
        backend::GpuHash160Executor gpu(ordinal,htargets,verifier,options);
        const auto ticket=gpu.submit(hbatch);gpu.drain();const auto result=gpu.take(ticket);
        std::set<std::pair<UInt256,uint8_t>> found,wanted;
        for(const auto& scalar:expected)for(uint8_t tag:{1,2})wanted.emplace(scalar,tag);
        for(const auto& match:result.matches)found.emplace(match.scalar,htargets.values()[match.target][0]);
        if(result.overflow||result.verified_steps!=33||found!=wanted||result.matches.size()!=wanted.size())
            throw std::runtime_error("GPU HASH160 runtime self-test failed");
    }
    std::vector<EthereumTarget> addresses;
    for(const auto& point:points)addresses.push_back(ethereum_target(point));
    const EthereumTargets etargets(std::move(addresses));
    identity.algorithm=scheduler::WorkAlgorithm::DirectEthereumV1;identity.target_digest=etargets.digest();
    const auto ework=*scheduler::WorkUnit::plan(grid,UInt256(),UInt256(1),33,identity);
    const auto ebatch=*scheduler::KernelBatch::plan(ework,UInt256(1),33);
    // Check the Keccak path on this ordinal before accepting any new grant.
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped,backend::XPointKernel::Glv}){
        backend::EthereumOptions options;options.max_steps=64;options.kernel=kernel;options.candidate_capacity=16;
        backend::GpuEthereumExecutor gpu(ordinal,etargets,verifier,options);
        const auto ticket=gpu.submit(ebatch);gpu.drain();const auto result=gpu.take(ticket);
        std::set<UInt256> found;for(const auto& match:result.matches)found.insert(match.scalar);
        if(result.overflow||result.verified_steps!=33||found!=expected||result.matches.size()!=expected.size())
            throw std::runtime_error("GPU Ethereum runtime self-test failed");
    }
    std::vector<VanityTarget> prefixes{vanity_target("1",1),vanity_target("1",2)};
    for(const auto& point:points)for(uint8_t tag:{1,2})
        prefixes.push_back(vanity_target(bitcoin_address(point,tag).substr(0,8),tag));
    const VanityTargets vtargets(std::move(prefixes));
    identity.algorithm=scheduler::WorkAlgorithm::DirectVanityV1;identity.target_digest=vtargets.digest();
    const auto vwork=*scheduler::WorkUnit::plan(grid,UInt256(),UInt256(1),33,identity);
    const auto vbatch=*scheduler::KernelBatch::plan(vwork,UInt256(1),33);
    // Broad and longer prefixes deliberately overlap; losing a relation must
    // fail the fresh per-device startup gate just like losing a scalar would.
    std::set<std::pair<UInt256,uint32_t>> wanted;
    for(unsigned scalar=1;scalar<=33;++scalar)for(uint32_t t=0;t<vtargets.values().size();++t){
        const auto& prefix=vtargets.values()[t];
        if(vanity_matches(bitcoin_address(verifier.derive(UInt256(scalar)),prefix[0]),prefix))wanted.emplace(UInt256(scalar),t);
    }
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped,backend::XPointKernel::Glv}){
        backend::VanityOptions options;options.max_steps=64;options.kernel=kernel;options.candidate_capacity=128;
        backend::GpuVanityExecutor gpu(ordinal,vtargets,verifier,options);
        const auto ticket=gpu.submit(vbatch);gpu.drain();const auto result=gpu.take(ticket);
        std::set<std::pair<UInt256,uint32_t>> found;for(const auto& match:result.matches)found.emplace(match.scalar,match.target);
        if(result.overflow||result.verified_steps!=33||found!=wanted||result.matches.size()!=wanted.size())
            throw std::runtime_error("GPU vanity runtime self-test failed");
    }
    stride_self_test<backend::GpuXPointExecutor>(ordinal,targets,scheduler::WorkAlgorithm::DirectXPointV1,verifier,
        [](const auto& pub,const auto& target){return std::equal(target.begin(),target.end(),pub.begin()+1);});
    stride_self_test<backend::GpuHash160Executor>(ordinal,htargets,scheduler::WorkAlgorithm::DirectHash160V1,verifier,
        [](const auto& pub,const auto& target){return hash160_target(pub,target[0])==target;});
    stride_self_test<backend::GpuEthereumExecutor>(ordinal,etargets,scheduler::WorkAlgorithm::DirectEthereumV1,verifier,
        [](const auto& pub,const auto& target){return ethereum_target(pub)==target;});
    stride_self_test<backend::GpuVanityExecutor>(ordinal,vtargets,scheduler::WorkAlgorithm::DirectVanityV1,verifier,
        [](const auto& pub,const auto& target){return vanity_matches(bitcoin_address(pub,target[0]),target);});
    // Published minikeys exercise both lengths and encodings. Nearby rejected
    // candidates still count toward exact ordinal coverage in this fresh process.
    for(const char* text:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"}){
        const auto begin=minikey_ordinal(text);const auto point=verifier.derive(*minikey_scalar(text));
        const MinikeyTargets mt({minikey_target(unsigned(std::string(text).size()),hash160_target(point,1)),
            minikey_target(unsigned(std::string(text).size()),hash160_target(point,2))});
        const scheduler::BlockGrid mg(ScalarInterval(begin,begin.add(UInt256(257))),UInt256(257));
        identity.algorithm=scheduler::WorkAlgorithm::DirectMinikeysV1;identity.target_digest=mt.digest();
        const auto mw=*scheduler::WorkUnit::plan(mg,UInt256(),begin,257,identity);
        const auto mb=*scheduler::KernelBatch::plan(mw,begin,257);
        backend::MinikeysOptions options;options.max_steps=257;options.candidate_capacity=2;
        backend::GpuMinikeysExecutor gpu(ordinal,mt,verifier,options);
        const auto ticket=gpu.submit(mb);gpu.drain();const auto result=gpu.take(ticket);
        std::set<std::pair<UInt256,uint32_t>> found;
        for(const auto& match:result.matches)found.emplace(match.scalar,match.target);
        if(result.overflow||result.verified_steps!=257||result.matches.size()!=2||
            found!=std::set<std::pair<UInt256,uint32_t>>{{begin,0},{begin,1}})
            throw std::runtime_error("GPU minikey runtime self-test failed");
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
