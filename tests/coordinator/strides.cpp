#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    for(const auto mode:{Mode::XPoint,Mode::Hash160,Mode::Ethereum,Mode::Vanity}){
        Temporary server,local;int64_t now=1800000000,monotonic=boot_seconds();
        Repository repo(server.path.string(),[&]{return now;});
        const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
        const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
        const auto project=repo.admin({{"operation","project-create"},{"name","strides"},{"owner",client["client"]}})["project"].get<std::string>();
        core::XPointVerifier verifier;const auto pub=verifier.derive(UInt256(101));core::XPointBytes x{};std::copy_n(pub.begin()+1,32,x.begin());
        const core::XPointTargets xt({x});const core::Hash160Targets ht({core::hash160_target(pub,1),core::hash160_target(pub,2)});
        const core::EthereumTargets et({core::ethereum_target(pub)});
        const core::VanityTargets vt({core::vanity_target(core::bitcoin_address(pub,1),1),core::vanity_target(core::bitcoin_address(pub,2),2)});
        const core::ScalarStride mapping({UInt256(101),UInt256(339)},UInt256(7));
        const auto input=with_stride(mode==Mode::XPoint?binding(xt):mode==Mode::Hash160?binding(ht):mode==Mode::Ethereum?binding(et):binding(vt),mapping);
        const Json body{{"mode",mode_name(mode)},{"begin",UInt256(1).hex()},{"end_exclusive",UInt256(35).hex()},
            {"block_width",UInt256(17).hex()},{"configuration",wire::hex(input.configuration)},{"targets",wire::hex(input.targets)}};
        const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",body);
        auto invalid=body;invalid["end_exclusive"]=UInt256(36).hex();
        rejects([&]{repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",invalid);});
        const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
        const Json jobs={{{"project",project},{"job",job["job"]},{"devices",{"gpu0"}},{"spares",1},{"policy","sequential"}}};
        Json old{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1","hash160-v1","ethereum-v1","vanity-v1","minikeys-v1"}},
            {"instance","old-worker"},{"request","old-request"},{"jobs",jobs},{"updates",Json::array()},{"returns",Json::array()}};
        for(unsigned count=6;count>=2;--count){old["capabilities"].erase(old["capabilities"].begin()+count,old["capabilities"].end());
            denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",old);});
            require(repo.request(cert,"GET",path+"/status")["assignments"]==0,"old worker reserved candidate-index work");}
        Worker worker(local.path.string(),[&]{return now;},[&]{return monotonic;});
        worker.configure({{"endpoint","https://test.invalid"},{"ca","/private/ca"},{"certificate","/private/certificate"},{"key","/private/key"},{"jobs",jobs}});
        Json request;
        const auto transport=[&](const Json& sent){request=sent;
            require(sent["capabilities"].back()=="scalar-stride-v1","worker omitted stride capability");
            return Json{{"ok",true},{"server_time",now},{"value",repo.request(cert,"POST","/api/v1/sync",sent)},{"controls",repo.control_snapshot(cert,sent)}};
        };
        rejects([&]{worker.synchronize([&](const Json& sent){auto response=transport(sent);
            auto config=input.configuration;config.back()=8;response["value"]["jobs"][0]["configuration"]=wire::hex(config);return response;});});
        require(!worker.next("gpu0"),"altered remote mapping imported");worker.synchronize(transport,true);
        auto downgrade=request;downgrade["capabilities"].erase(downgrade["capabilities"].end()-1);
        denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",downgrade);});
        unsigned completed=0;
        while(auto grant=worker.next("gpu0")){
            require(worker.journal().stride_mapping(grant->scope)==input.stride_mapping,"worker lost mapping");
            CheckpointOptions options;options.xpoint_steps=5;options.checkpoint_seconds=0;
            const auto runner=[&](const auto& batch){backend::XPointResult result{batch,{}};result.device_steps=result.verified_steps=batch.step_count();
                if(batch.interval().contains(UInt256(1)))for(uint32_t t=0;t<input.count();++t)result.matches.push_back({UInt256(1),t});
                result.candidate_count=result.matches.size();return result;};
            CheckpointSummary result;
            if(mode==Mode::XPoint)result=CheckpointRun::xpoint(worker.journal(),*grant,xt,verifier,runner,options);
            if(mode==Mode::Hash160)result=CheckpointRun::hash160(worker.journal(),*grant,ht,verifier,runner,options);
            if(mode==Mode::Ethereum)result=CheckpointRun::ethereum(worker.journal(),*grant,et,verifier,runner,options);
            if(mode==Mode::Vanity)result=CheckpointRun::vanity(worker.journal(),*grant,vt,verifier,runner,options);
            require(result.complete&&result.computed_scalars==UInt256(17),"candidate grant incomplete");++completed;
        }
        require(completed==2&&repo.request(cert,"GET",path+"/results").empty(),"offline reservation/coverage mismatch");
        Json pending;
        rejects([&]{worker.synchronize([&](const Json& sent)->Json{pending=sent;transport(sent);throw std::runtime_error("lost upload reply");},true);});
        const auto rows=repo.request(cert,"GET",path+"/results");require(rows.size()==input.count(),"server lost mapped relations");
        for(const auto& row:rows)require(row["candidate_index"]==UInt256(1).hex()&&row["scalar"]==UInt256(101).hex()&&
            row["coordinate_space"]=="scalar-stride-index-v1","public result confused index and scalar");
        auto stale=pending;stale["capabilities"].erase(stale["capabilities"].end()-1);
        denied(426,[&]{repo.request(cert,"POST","/api/v1/sync",stale);});
        worker.synchronize([&](const Json& sent){require(sent==pending,"pending request changed");return transport(sent);},true);
        require(worker.status()["outbox_bytes"]==0&&!worker.next("gpu0"),"acknowledged outbox retained");
        require(repo.request(cert,"GET",path+"/results")==rows,"retry duplicated mapped results");
        worker.journal().check();repo.admin({{"operation","check"}});
    }
    std::cout<<"Four-family stride capability fencing, canonical import and durable upload retry passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
