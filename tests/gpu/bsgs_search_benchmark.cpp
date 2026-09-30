// Equivalent target sets, table and tile for both kernels. One warm-up and five
// alternating samples; validate the expected match set before accepting timing.
#include "keyhunt/backend/hip_bsgs.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
using namespace keyhunt;
using core::UInt256;
int main(int argc,char** argv){
    try {
        if(argc>4)throw std::invalid_argument("usage: hip_bsgs_search_benchmark [device] [m] [giants]");
        const int device=argc>1?std::stoi(argv[1]):0;
        const uint64_t m=argc>2?std::stoull(argv[2]):65537,giants=argc>3?std::stoull(argv[3]):32768;
        if(!m || m>1048576 || !giants || giants>32768)throw std::invalid_argument("m in [1,1048576], giants in [1,32768]");
        using Clock=std::chrono::steady_clock;
        const auto building=Clock::now();const auto table=bsgs::Table::build(m);
        const double build_ms=std::chrono::duration<double,std::milli>(Clock::now()-building).count();
        core::XPointVerifier cpu;
        const auto begin=UInt256::from_hex("800000000000000000000000000000000000000000000000fffffffffffffff1");
        const uint64_t width=m*giants;
        const core::ScalarInterval interval(begin,begin.add(UInt256(width)));
        std::cout<<std::setprecision(9)<<"{\"device\":"<<device<<",\"m\":"<<m<<",\"giants_per_target\":"<<giants
            <<",\"scalar_width\":"<<width<<",\"begin\":\""<<begin.hex()<<"\",\"durable_coverage\":false,\"table_build_ms\":"<<build_ms<<",\"workloads\":[";
        for(unsigned workload=0;workload<3;++workload){
            std::vector<core::UncompressedPublicKey> points;std::vector<UInt256> expected;
            if(workload==1){
                for(uint64_t offset:{uint64_t(0),width/2,width-1})expected.push_back(begin.add(UInt256(offset)));
                std::sort(expected.begin(),expected.end());expected.erase(std::unique(expected.begin(),expected.end()),expected.end());
                for(const auto& scalar:expected)points.push_back(cpu.derive(scalar));
            }else{
                // These known finite points have their unique scalars outside
                // this interval, proving an empty expected set independently of lookup.
                for(unsigned k=1;k<=(workload==0?1U:32U);++k)points.push_back(cpu.derive(UInt256(k)));
            }
            core::BsgsPublicKeyTargets targets(points);
            core::BsgsBatch batch(interval,m,0,uint32_t(targets.values().size()),targets.digest(),table.checksum());
            std::unique_ptr<backend::HipBsgsExecutor> owners[3];double prep[3]{};
            for(unsigned kind=0;kind<3;++kind){
                backend::BsgsSearchOptions options;options.group_size=kind==2?0:kind?8:1;
                const auto start=Clock::now();owners[kind]=std::make_unique<backend::HipBsgsExecutor>(device,table,targets,cpu,options);
                prep[kind]=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            }
            std::cout<<(workload?",":"")<<"{\"name\":\""<<(workload==0?"no_match_1":workload==1?"boundary_3":"no_match_32")
                <<"\",\"targets\":"<<targets.values().size()<<",\"prepare_single_ms\":"<<prep[0]<<",\"prepare_grouped_ms\":"<<prep[1]
                <<",\"prepare_auto_ms\":"<<prep[2]<<",\"upload_auto_ms\":"<<owners[2]->table_upload_ms()<<",\"upload_single_ms\":"<<owners[0]->table_upload_ms()<<",\"upload_grouped_ms\":"<<owners[1]->table_upload_ms()<<",\"samples\":[";
            bool first=true;
            for(int sample=-1;sample<5;++sample)for(unsigned order=0;order<3;++order){
                const unsigned kind=(order+unsigned(sample+1))%3;
                auto& owner=*owners[kind];auto ticket=owner.submit(batch);owner.drain();const auto result=owner.take(ticket);
                if(result.overflow || result.verified_steps!=batch.steps() || result.matches.size()!=expected.size())
                    throw std::runtime_error("benchmark coverage/match count differs");
                for(size_t i=0;i<expected.size();++i)if(result.matches[i].scalar!=expected[i])throw std::runtime_error("benchmark match differs");
                std::cout<<(first?"":",")<<"{\"group_size\":"<<(kind==2?0:kind?8:1)<<",\"dispatched_group\":"<<result.group_size<<",\"sample\":"<<sample<<",\"kernel_ms\":"<<result.kernel_ms
                    <<",\"download_ms\":"<<result.download_ms<<",\"seed_ms\":"<<result.seed_ms<<",\"verification_ms\":"<<result.verification_ms
                    <<",\"wall_ms\":"<<result.wall_ms<<",\"device_steps\":"<<result.device_steps<<",\"matches\":"<<result.matches.size()<<'}';first=false;
            }
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
