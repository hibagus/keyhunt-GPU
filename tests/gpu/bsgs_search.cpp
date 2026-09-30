#include "keyhunt/backend/hip_bsgs.h"
#include <iostream>
using namespace keyhunt;
using core::UInt256;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
    try {
        core::XPointVerifier cpu;
        auto table=bsgs::Table::build(7);
        std::vector<core::UncompressedPublicKey> points;
        for(uint64_t k=1;k<=130;++k)points.push_back(cpu.derive(UInt256(k)));
        core::BsgsTargets targets(points);
        const core::ScalarInterval range(UInt256(1),UInt256(130));
        const auto make=[&](uint32_t first,uint32_t count){return core::BsgsBatch(range,7,first,count,targets.digest(),table.checksum());};
        auto batch=make(0,64);
        backend::BsgsSearchOptions options; options.candidate_capacity=2;
        for(unsigned group:{0U,1U,8U}){
            options.group_size=group;
            backend::HipBsgsExecutor executor(0,table,targets,cpu,options),other(0,table,targets,cpu,options);
            auto wrong=targets.digest();wrong[0]^=1;
            rejects([&]{executor.submit(core::BsgsBatch(range,7,0,1,wrong,table.checksum()));});
            rejects([&]{executor.submit(core::BsgsBatch(range,8,0,1,targets.digest(),table.checksum()));});
            rejects([&]{executor.submit(core::BsgsBatch(range,7,0,1,targets.digest(),{}));});
            rejects([&]{executor.submit(make(129,2));});
            const auto ticket=executor.submit(batch);
            rejects([&]{executor.submit(batch);}); rejects([&]{other.poll(ticket);});
            executor.drain(); require(executor.poll(ticket),"not ready after drain");
            auto overflow=executor.take(ticket);
            require(overflow.group_size==1 || overflow.group_size==8,"invalid dispatched kernel");
            if(group)require(overflow.group_size==group,"explicit group ignored");
            require(overflow.overflow && overflow.matches.empty() && !overflow.verified_steps,"overflow credited");
            rejects([&]{executor.take(ticket);});
            uint64_t found=0,steps=0;
            // Replay every target in bounded subsets; no early exit on a hit.
            for(uint32_t first=0;first<130;first+=2){
                auto t=executor.submit(make(first,2));executor.drain();auto result=executor.take(t);
                require(!result.overflow && result.verified_steps==38,"replay count");
                found+=result.matches.size(); steps+=result.verified_steps;
                // A consumed result owns its matches even when the slot is reused.
                if(first==0)require(!result.matches.empty(),"missing initial subset");
            }
            require(found==129 && steps==2470,"all-target replay coverage");
            {backend::HipBsgsExecutor pending(0,table,targets,cpu,options);pending.submit(make(0,1));}
        }
        options.host_memory_bytes=1;rejects([&]{backend::HipBsgsExecutor e(0,table,targets,cpu,options);});
        options={};options.memory_reserve_bytes=UINT64_MAX;rejects([&]{backend::HipBsgsExecutor e(0,table,targets,cpu,options);});
        options={};options.max_steps=1;backend::HipBsgsExecutor small(0,table,targets,cpu,options);rejects([&]{small.submit(batch);});
        options={};options.group_size=2;rejects([&]{backend::HipBsgsExecutor e(0,table,targets,cpu,options);});
        std::cout<<"HIP BSGS slot identity, overflow/replay, all targets and ownership passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
