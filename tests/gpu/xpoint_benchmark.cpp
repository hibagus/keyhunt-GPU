// Warm executors with immutable targets, then alternate equivalent direct and
// stepped searches. Every measured result is CPU verified and its expected match
// scalars/count checked. This is a single-device, volatile-coverage benchmark.
#include "keyhunt/backend/hip_xpoint.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
using namespace keyhunt;
using core::UInt256;
int main(int argc,char** argv) {
    try {
        if (argc>3) throw std::invalid_argument("usage: hip_xpoint_benchmark [device] [steps]");
        const int device=argc>1 ? std::stoi(argv[1]) : 0;
        const uint64_t count=argc>2 ? std::stoull(argv[2]) : 1048576;
        if (!count || count>1048576) throw std::invalid_argument("steps must be in [1,1048576]");
        core::XPointVerifier verifier;
        const auto begin=UInt256::from_hex("800000000000000000000000000000000000000000000000fffffffffffffff1");
        std::cout<<std::setprecision(9)<<"{\"device\":"<<device<<",\"count\":"<<count<<",\"begin\":\""<<begin.hex()<<"\",\"workloads\":[";
        for (unsigned workload=0;workload<3;++workload) {
            std::vector<core::XPointBytes> values;
            std::vector<UInt256> expected;
            if (workload==1) {
                for (uint64_t offset : {uint64_t(0),count/2,count-1}) {
                    const auto scalar=begin.add(UInt256(offset));
                    const auto pub=verifier.derive(scalar);
                    core::XPointBytes x{}; std::copy_n(pub.begin()+1,32,x.begin()); values.push_back(x);
                    expected.push_back(scalar);
                }
                std::sort(expected.begin(),expected.end());
                expected.erase(std::unique(expected.begin(),expected.end()),expected.end());
            } else {
                // The direct baseline must confirm this presumed no-match set
                // before timings are accepted; every unexpected candidate fails.
                for (unsigned i=0;i<(workload==0 ? 1U : 32U);++i) values.push_back(UInt256(i).bytes());
            }
            core::XPointTargets targets(values);
            scheduler::ExecutionIdentity id; id.target_digest=targets.digest();
            id.assignment_id[0]=1; id.assignment_generation=id.executor_generation=1;
            scheduler::BlockGrid grid(core::ScalarInterval(begin,begin.add(UInt256(count))),UInt256(count));
            auto work=*scheduler::WorkUnit::plan(grid,UInt256(0),begin,count,id);
            auto batch=*scheduler::KernelBatch::plan(work,begin,count);
            std::unique_ptr<backend::HipXPointExecutor> owners[2];
            double preparation[2]{};
            for (unsigned kind=0;kind<2;++kind) {
                backend::XPointOptions options; options.max_steps=count; options.candidate_capacity=1024;
                options.kernel=kind ? backend::XPointKernel::Stepped : backend::XPointKernel::Direct;
                auto start=std::chrono::steady_clock::now();
                owners[kind]=std::make_unique<backend::HipXPointExecutor>(device,targets,verifier,options);
                preparation[kind]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            }
            if (workload) std::cout<<',';
            std::cout<<"{\"name\":\""<<(workload==0 ? "no_match_1" : workload==1 ? "boundary_3" : "no_match_32")
                     <<"\",\"targets\":"<<targets.values().size()<<",\"prepare_direct_ms\":"<<preparation[0]
                     <<",\"prepare_stepped_ms\":"<<preparation[1]<<",\"samples\":[";
            bool first=true;
            for (int sample=-1;sample<5;++sample) {
                for (unsigned order=0;order<2;++order) {
                    const unsigned kind=sample>=0 && sample%2 ? 1-order : order;
                    auto& owner=*owners[kind];
                    auto ticket=owner.submit(batch); owner.drain(); auto result=owner.take(ticket);
                    if (result.overflow || result.verified_steps!=count || result.matches.size()!=expected.size())
                        throw std::runtime_error("benchmark coverage/match count differs");
                    for (size_t i=0;i<expected.size();++i)
                        if (result.matches[i].scalar!=expected[i]) throw std::runtime_error("benchmark match differs");
                    if (sample<0) continue;
                    std::cout<<(first ? "" : ",")<<"{\"kernel\":\""<<(kind ? "stepped" : "direct")
                        <<"\",\"sample\":"<<sample<<",\"kernel_ms\":"<<result.kernel_ms
                        <<",\"download_ms\":"<<result.download_ms<<",\"seed_ms\":"<<result.seed_ms
                        <<",\"verification_ms\":"<<result.verification_ms<<",\"wall_ms\":"<<result.wall_ms
                        <<",\"device_steps\":"<<result.device_steps<<",\"matches\":"<<result.matches.size()<<'}';
                    first=false;
                }
            }
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
