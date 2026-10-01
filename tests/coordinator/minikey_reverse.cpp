#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
#include <set>
using namespace cfixture;using namespace keyhunt::storage::detail;
int main(int argc,char** argv){try{
 const bool both=argc==2&&std::string(argv[1])=="--both-ends";
 const bool dance=argc==2&&std::string(argv[1])=="--dance";
 const bool random=argc==2&&std::string(argv[1])=="--random-window";
 require(argc==1||both||dance||random,"unexpected test option");
 const auto orders=random?std::vector<core::MinikeyOrder>{core::MinikeyOrder::RandomWindow}:dance?std::vector<core::MinikeyOrder>{core::MinikeyOrder::Dance}:both?std::vector<core::MinikeyOrder>{core::MinikeyOrder::BothEnds}:
     std::vector<core::MinikeyOrder>{core::MinikeyOrder::Forward,core::MinikeyOrder::Reverse};
 for(const std::string key:{"SzavMBLoXU6kDrqtUVmffv","S6c56bnXQiBjk9mqSYE7ykVQ7NzrRy"})
 for(const auto order:orders)for(unsigned seconds:{0U,180U}){
    Temporary server,local;int64_t now=1800000000;Repository repo(server.path.string(),[&]{return now;});
    const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
    const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
    const auto project=repo.admin({{"operation","project-create"},{"name","ordinal gaps"},{"owner",client["client"]}})["project"].get<std::string>();
    core::XPointVerifier verifier;const auto begin=core::minikey_ordinal(key);const auto length=unsigned(key.size());
    auto later=begin.add(UInt256(191));while(!core::minikey_scalar(core::minikey_text(later,length)))later=later.add(UInt256(1));
    const auto end=later.add(UInt256(100));const auto width=end.subtract(begin);
    std::vector<core::MinikeyTarget> values;
    for(const auto& at:{begin,later})for(uint8_t tag:{1,2})values.push_back(core::minikey_target(length,core::hash160_target(verifier.derive(*core::minikey_scalar(core::minikey_text(at,length))),tag)));
    const core::MinikeyTargets targets(values);const auto input=binding(targets);
    const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",{{"mode","minikeys"},{"begin",begin.hex()},{"end_exclusive",end.hex()},
        {"block_width",width.hex()},{"configuration",wire::hex(input.configuration)},{"targets",wire::hex(input.targets)}});
    const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
    const Json jobs={{{"project",project},{"job",job["job"]},{"devices",{"gpu"}},{"spares",0},{"policy","sequential"}}};
    // The original minikey-capable worker has six capabilities. Reverse changes
    // neither the canonical ordinal receipts nor this compatibility boundary.
    Json request{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1","hash160-v1","ethereum-v1","vanity-v1","minikeys-v1"}},
        {"instance","original"},{"request","claim"},{"jobs",jobs},{"updates",Json::array()},{"returns",Json::array()}};
    const auto original=wire::grant(repo.request(cert,"POST","/api/v1/sync",request)["grants"][0]["grant"]);
    const std::vector<ScalarInterval> islands{{begin,begin.add(UInt256(1))},{begin.add(UInt256(32)),begin.add(UInt256(65))},{begin.add(UInt256(128)),begin.add(UInt256(191))}};
    std::vector<core::XPointMatch> prior;
    for(uint8_t tag:{1,2}){const auto target=core::minikey_target(length,core::hash160_target(verifier.derive(*core::minikey_scalar(key)),tag));
        prior.push_back({begin,uint32_t(std::lower_bound(targets.values().begin(),targets.values().end(),target)-targets.values().begin())});}
    const CheckpointData page{original.block,uint64_t(original.generation),1,original.epoch,islands,prior};
    request["request"]="partial";request["updates"]={{{"grant",wire::grant(original)},{"started",true},{"checkpoints",{wire::hex(encode_checkpoint(page))}}}};
    repo.request(cert,"POST","/api/v1/sync",request);now+=31*86400;
    Worker worker(local.path.string(),[&]{return now;});worker.configure({{"endpoint","https://test.invalid"},{"ca","/ca"},{"certificate","/cert"},{"key","/key"},{"jobs",jobs}});
    repo.request(cert,"POST",path+"/blocks/"+original.block.hex()+"/recover",{{"client",client["client"]},{"instance",worker.status()["instance"]},{"device","gpu"},{"request","recover"},{"previous_executor_stopped",true}});
    auto transport=[&](const Json& sent){require(sent["capabilities"].size()==9,"ordinal order changed capabilities");return Json{{"ok",true},{"server_time",now},{"value",repo.request(cert,"POST","/api/v1/sync",sent)}};};
    worker.synchronize(transport);const auto grant=*worker.next("gpu");std::set<UInt256> missing;
    for(const auto& gap:worker.journal().block(grant.scope,grant.block).remaining)for(auto at=gap.begin();at<gap.end();at=at.add(UInt256(1)))missing.insert(at);
    CheckpointOptions options;options.minikey_order=order;options.xpoint_steps=17;options.work_unit_seconds=seconds;options.checkpoint_seconds=0;
    if(random)options.minikey_random_window=core::MinikeyRandomWindow{UInt256(42),4};
    unsigned phase=0;
    const auto pivot=missing.begin()->add(missing.rbegin()->add(UInt256(1)).subtract(*missing.begin()).divmod(UInt256(2)).first);
    const auto summary=CheckpointRun::minikeys(worker.journal(),grant,targets,verifier,[&](const auto& batch){
        const bool reverse=order==core::MinikeyOrder::Reverse || (both&&phase%2==1) || (dance&&phase%3==1);
        require(batch.ordinal_reverse()==reverse,"recovery direction lost");
        backend::MinikeysResult result{batch,{}};result.device_steps=result.verified_steps=batch.step_count();
        for(uint64_t i=0;i<batch.step_count();++i){const auto at=batch.ordinal_at(i);
            require(!missing.empty(),"repeated completed coverage");
            auto expected=reverse?*missing.rbegin():*missing.begin();
            if(dance&&phase%3==2){const auto middle=missing.lower_bound(pivot);if(middle!=missing.end())expected=*middle;}
            if(!random)require(at==expected,"wrong selected ordinal front");
            require(missing.erase(at),"random-window repeated accepted coverage");
            const auto scalar=core::minikey_scalar(core::minikey_text(at,length));if(!scalar)continue;
            const auto point=verifier.derive(*scalar);
            for(uint8_t tag:{1,2}){const auto target=core::minikey_target(length,core::hash160_target(point,tag));const auto found=std::lower_bound(targets.values().begin(),targets.values().end(),target);
                if(found!=targets.values().end()&&*found==target)result.matches.push_back({at,uint32_t(found-targets.values().begin())});}}
        result.candidate_count=result.matches.size();++phase;return result;
    },options);
    require(summary.complete&&missing.empty()&&summary.resumed_scalars==UInt256(97)&&summary.computed_scalars==width.subtract(UInt256(97)),"fragmented ordinal union changed");
    require(worker.journal().results(grant.scope).size()==2,"old owner results recomputed");
    Json pending;rejects([&]{worker.synchronize([&](const Json& sent)->Json{pending=sent;transport(sent);throw std::runtime_error("lost upload reply");},true);});
    const auto rows=repo.request(cert,"GET",path+"/results");require(rows.size()==4,"server lost result union");
    for(const auto& row:rows)require(row["ordinal"]==begin.hex()||row["ordinal"]==later.hex(),"result ordinal changed");
    worker.synchronize([&](const Json& sent){require(sent==pending,"pending upload changed");return transport(sent);},true);
    require(repo.request(cert,"GET",path+"/results")==rows&&worker.status()["outbox_bytes"]==0&&!worker.next("gpu"),"retry was not idempotent");
    worker.journal().check();repo.admin({{"operation","check"}});
 }
 std::cout<<"Minikey fragmented recovery, old-worker capabilities, both directions and upload replay passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
