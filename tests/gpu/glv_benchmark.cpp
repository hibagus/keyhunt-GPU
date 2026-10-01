// Equivalent warm searches, with rotating execution order to avoid consistently
// giving one kernel a colder GPU. Every sample checks exact coverage and scalars;
// the executors independently verify returned public-key relations on the CPU.
#include "keyhunt/backend/gpu_xpoint.h"
#include "keyhunt/backend/gpu_hash160.h"
#include "keyhunt/backend/gpu_ethereum.h"
#include "keyhunt/backend/gpu_vanity.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace keyhunt;
using core::UInt256;
namespace {
constexpr backend::XPointKernel kernels[]={backend::XPointKernel::Direct,backend::XPointKernel::Glv,backend::XPointKernel::Stepped};
constexpr const char* names[]={"direct","glv","stepped"};
template<class Executor,class Targets>
void measure(int device,uint64_t count,const char* family,const char* region,UInt256 begin,
             const Targets& targets,const core::XPointVerifier& verifier,scheduler::WorkAlgorithm algorithm,
             unsigned per_scalar,bool& first) {
    scheduler::ExecutionIdentity id;id.algorithm=algorithm;id.target_digest=targets.digest();
    id.assignment_id[0]=1;id.assignment_generation=id.executor_generation=1;
    scheduler::BlockGrid grid(core::ScalarInterval(begin,begin.add(UInt256(count))),UInt256(count));
    const auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),begin,count,id);
    const auto batch=*scheduler::KernelBatch::plan(work,begin,count);
    std::unique_ptr<Executor> owners[3];double preparation[3]{};
    for(unsigned kind=0;kind<3;++kind){
        backend::XPointOptions options;options.max_steps=count;options.candidate_capacity=16;options.kernel=kernels[kind];
        const auto start=std::chrono::steady_clock::now();
        owners[kind]=std::make_unique<Executor>(device,targets,verifier,options);
        preparation[kind]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    }
    std::cout<<(first?"":",")<<"{\"family\":\""<<family<<"\",\"region\":\""<<region<<"\",\"begin\":\""<<begin.hex()
        <<"\",\"count\":"<<count<<",\"targets\":"<<targets.values().size()<<",\"preparation_ms\":["<<preparation[0]<<','<<preparation[1]<<','<<preparation[2]<<"],\"samples\":[";
    first=false;bool first_sample=true;
    // Two untimed rounds allow module loading, clocks and caches to settle.
    for(unsigned round=0;round<11;++round)for(unsigned order=0;order<3;++order){
        const unsigned kind=(round+order)%3;auto& owner=*owners[kind];
        const auto ticket=owner.submit(batch);owner.drain();const auto result=owner.take(ticket);
        if(result.overflow || result.verified_steps!=count || result.device_steps!=count || result.matches.size()!=2*per_scalar)
            throw std::runtime_error("benchmark coverage or match count differs");
        for(const auto scalar:{begin,begin.add(UInt256(count-1))})
            if(std::count_if(result.matches.begin(),result.matches.end(),[&](const auto& match){return match.scalar==scalar;})!=per_scalar)
                throw std::runtime_error("benchmark boundary match differs");
        if(round<2)continue;
        std::cout<<(first_sample?"":",")<<"{\"kernel\":\""<<names[kind]<<"\",\"round\":"<<round-2
            <<",\"order\":"<<order<<",\"kernel_ms\":"<<result.kernel_ms<<",\"wall_ms\":"<<result.wall_ms
            <<",\"download_ms\":"<<result.download_ms<<",\"verification_ms\":"<<result.verification_ms
            <<",\"seed_ms\":"<<result.seed_ms<<",\"download_bytes\":"<<result.download_bytes
            <<",\"device_allocation_bytes\":"<<result.device_allocation_bytes<<",\"matches\":"<<result.matches.size()<<'}';
        first_sample=false;
    }
    std::cout<<"]}";
}
}
int main(int argc,char** argv){
    try{
        if(argc>3)throw std::invalid_argument("usage: glv_benchmark [device] [steps]");
        const int device=argc>1?std::stoi(argv[1]):0;
        const uint64_t count=argc>2?std::stoull(argv[2]):65536;
        if(!count || count<2 || count>1048576)throw std::invalid_argument("steps must be in [2,1048576]");
        core::XPointVerifier verifier;bool first=true;
        std::cout<<std::setprecision(9)<<"{\"device\":"<<device<<",\"kernel_order\":[\"direct\",\"glv\",\"stepped\"],\"warmup_rounds\":2,\"measured_rounds\":9,\"workloads\":[";
        for(const auto& region:{std::pair{"low",UInt256(1)},std::pair{"bit128",UInt256::from_hex("100000000000000000000000000000000")},
              std::pair{"bit192",UInt256::from_hex("1000000000000000000000000000000000000000000000000")},
              std::pair{"dense256",UInt256::from_hex("6a09e667f3bcc908bb67ae8584caa73b3c6ef372fe94f82ba54ff53a5f1d36f1")},
              std::pair{"order",core::scalar_order().subtract(UInt256(count))}}){
            const auto begin=region.second;
            std::vector<core::XPointBytes> x;std::vector<core::Hash160Target> h;
            std::vector<core::EthereumTarget> e;std::vector<core::VanityTarget> v;
            for(const auto scalar:{begin,begin.add(UInt256(count-1))}){
                const auto pub=verifier.derive(scalar);core::XPointBytes point{};std::copy_n(pub.begin()+1,32,point.begin());x.push_back(point);
                e.push_back(core::ethereum_target(pub));
                for(uint8_t encoding:{1,2}){h.push_back(core::hash160_target(pub,encoding));v.push_back(core::vanity_target(core::bitcoin_address(pub,encoding),encoding));}
            }
            measure<backend::GpuXPointExecutor>(device,count,"xpoint",region.first,begin,core::XPointTargets(x),verifier,scheduler::WorkAlgorithm::DirectXPointV1,1,first);
            measure<backend::GpuHash160Executor>(device,count,"hash160",region.first,begin,core::Hash160Targets(h),verifier,scheduler::WorkAlgorithm::DirectHash160V1,2,first);
            measure<backend::GpuEthereumExecutor>(device,count,"ethereum",region.first,begin,core::EthereumTargets(e),verifier,scheduler::WorkAlgorithm::DirectEthereumV1,1,first);
            measure<backend::GpuVanityExecutor>(device,count,"vanity",region.first,begin,core::VanityTargets(v),verifier,scheduler::WorkAlgorithm::DirectVanityV1,2,first);
        }
        std::cout<<"]}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
