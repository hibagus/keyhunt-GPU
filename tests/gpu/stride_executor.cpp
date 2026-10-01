#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/backend/gpu_hash160.h"
#include "keyhunt/backend/gpu_ethereum.h"
#include "keyhunt/backend/gpu_vanity.h"
#include <algorithm>
#include <iostream>
using namespace keyhunt;
using core::UInt256;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
template<class Executor,class Targets>
void exercise(const Targets& targets,scheduler::WorkAlgorithm algorithm,const core::XPointVerifier& verifier){
    const core::ScalarStride mapping({UInt256(101),UInt256(601)},UInt256(7));
    scheduler::ExecutionIdentity id;id.algorithm=scheduler::strided_algorithm(algorithm);id.stride_mapping=mapping;
    id.target_digest=targets.digest();id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    scheduler::BlockGrid grid(mapping.indices(),UInt256(72));
    const auto batch=[&](const auto& identity,UInt256 cursor,uint64_t count){
        const auto work=*scheduler::WorkUnit::plan(grid,UInt256(),cursor,count,identity);
        return *scheduler::KernelBatch::plan(work,cursor,count);
    };
    backend::XPointOptions options;options.stride=UInt256(7);options.max_steps=33;options.candidate_capacity=1024;
    for(auto kernel:{backend::XPointKernel::Direct,backend::XPointKernel::Stepped}){
        options.kernel=kernel;Executor executor(0,targets,verifier,options);
        // A prepared SG cache must never execute work from another progression.
        auto wrong=id;wrong.stride_mapping=core::ScalarStride({UInt256(101),UInt256(677)},UInt256(8));
        rejects([&]{executor.submit(batch(wrong,UInt256(1),1));});
        wrong=id;wrong.stride_mapping.reset();wrong.algorithm=algorithm;
        rejects([&]{executor.submit(batch(wrong,UInt256(1),1));});
        for(auto cursor:{1U,18U,40U}){
            const auto submitted=batch(id,UInt256(cursor),33);const auto ticket=executor.submit(submitted);
            executor.drain();const auto result=executor.take(ticket);
            require(result.verified_steps==33 && !result.overflow && result.batch.work().identity()==id,"mapped receipt mismatch");
            for(const auto& match:result.matches)require(submitted.interval().contains(match.scalar),"receipt lost candidate coordinates");
            require(!result.matches.empty(),"mapped targets were not found");
        }
        // Origin changes are safe when the step is unchanged: the host seed is
        // refreshed for each batch, while the cached multiples still describe SG.
        auto shifted=id;shifted.stride_mapping=core::ScalarStride({UInt256(108),UInt256(608)},UInt256(7));
        const auto ticket=executor.submit(batch(shifted,UInt256(1),1));executor.drain();
        require(!executor.take(ticket).matches.empty(),"stale progression origin");
    }
    for(auto invalid:{UInt256(),core::scalar_order()}){
        options.stride=invalid;rejects([&]{Executor executor(0,targets,verifier,options);});
    }
}
int main(){try{
    core::XPointVerifier verifier;std::vector<core::XPointBytes> xv;std::vector<core::Hash160Target> hv;
    std::vector<core::EthereumTarget> ev;std::vector<core::VanityTarget> vv;
    for(unsigned i=0;i<72;++i){const auto pub=verifier.derive(UInt256(101+7*i));
        core::XPointBytes x{};std::copy_n(pub.begin()+1,32,x.begin());xv.push_back(x);
        ev.push_back(core::ethereum_target(pub));
        for(unsigned encoding:{1,2}){hv.push_back(core::hash160_target(pub,encoding));vv.push_back(core::vanity_target(core::bitcoin_address(pub,encoding),encoding));}
    }
    exercise<backend::GpuXPointExecutor>(core::XPointTargets(xv),scheduler::WorkAlgorithm::DirectXPointV1,verifier);
    exercise<backend::GpuHash160Executor>(core::Hash160Targets(hv),scheduler::WorkAlgorithm::DirectHash160V1,verifier);
    exercise<backend::GpuEthereumExecutor>(core::EthereumTargets(ev),scheduler::WorkAlgorithm::DirectEthereumV1,verifier);
    exercise<backend::GpuVanityExecutor>(core::VanityTargets(vv),scheduler::WorkAlgorithm::DirectVanityV1,verifier);
    std::cout<<"Four-family stride ownership and cached-step binding passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
