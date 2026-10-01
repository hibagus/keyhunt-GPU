#include "fixture.h"
#include "keyhunt/coordinator/worker.h"
#include <iostream>
#include <set>
using namespace cfixture;
using namespace keyhunt::storage::detail;
int main(){try{
    for(bool reverse:{false,true}){
        Temporary server,local;int64_t now=1800000000;
        Repository repo(server.path.string(),[&]{return now;});
        const auto leaf=pem(1,now-60,now+90*86400);const auto cert=certificate(leaf);
        const auto client=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",leaf}});
        const auto project=repo.admin({{"operation","project-create"},{"name","BSGS gaps"},{"owner",client["client"]}})["project"].get<std::string>();
        core::XPointVerifier verifier;const auto targets=b_targets(verifier,{101,102,120,137,156,157,1000});
        const auto table=bsgs::Table::build(7);const auto input=binding(targets,table);
        const auto job=repo.request(cert,"POST","/api/v1/projects/"+project+"/jobs",{
            {"mode","bsgs"},{"begin",UInt256(101).hex()},{"end_exclusive",UInt256(158).hex()},
            {"block_width",UInt256(57).hex()},{"configuration",wire::hex(input.configuration)},{"targets",wire::hex(input.targets)}});
        const auto path="/api/v1/projects/"+project+"/jobs/"+job["job"].get<std::string>();
        const Json jobs={{{"project",project},{"job",job["job"]},{"devices",{"gpu"}},{"spares",0},{"policy","sequential"}}};
        // An original BSGS worker needs only the existing two capabilities.
        // Publish truthful disjoint receipts through the public sync protocol,
        // then recover the expired grant onto a fresh worker's journal.
        Json request{{"protocol",1},{"capabilities",{"checkpoint-v1","offline-lease-v1"}},
            {"instance","original"},{"request","claim"},{"jobs",jobs},{"updates",Json::array()},{"returns",Json::array()}};
        const auto original=wire::grant(repo.request(cert,"POST","/api/v1/sync",request)["grants"][0]["grant"]);
        const std::vector<ScalarInterval> islands{{UInt256(112),UInt256(119)},{UInt256(133),UInt256(141)},{UInt256(149),UInt256(153)}};
        const auto pub=verifier.derive(UInt256(137));
        const auto target=uint32_t(std::lower_bound(targets.values().begin(),targets.values().end(),pub)-targets.values().begin());
        const CheckpointData page{original.block,uint64_t(original.generation),1,original.epoch,islands,{{UInt256(137),target}}};
        request["request"]="partial";request["updates"]={{{"grant",wire::grant(original)},{"started",true},{"checkpoints",{wire::hex(encode_checkpoint(page))}}}};
        repo.request(cert,"POST","/api/v1/sync",request);now+=31*86400;
        Worker worker(local.path.string(),[&]{return now;});
        worker.configure({{"endpoint","https://test.invalid"},{"ca","/ca"},{"certificate","/cert"},{"key","/key"},{"jobs",jobs}});
        repo.request(cert,"POST",path+"/blocks/"+original.block.hex()+"/recover",{
            {"client",client["client"]},{"instance",worker.status()["instance"]},{"device","gpu"},
            {"request","transfer"},{"previous_executor_stopped",true}});
        const auto transport=[&](const Json& sent){
            require(sent["capabilities"].size()==9,"tile order changed semantic capabilities");
            const auto value=repo.request(cert,"POST","/api/v1/sync",sent);
            return Json{{"ok",true},{"server_time",now},{"value",value}};
        };
        worker.synchronize(transport);const auto grant=*worker.next("gpu");
        const auto before=worker.journal().block(grant.scope,grant.block);
        require(before.covered.size()==3&&before.remaining.size()==4,"fragmented recovery fixture lost islands");
        CheckpointOptions options;options.giant_steps=1;options.target_batch=2;options.checkpoint_seconds=0;
        options.bsgs_reverse_tiles=reverse;std::optional<ScalarInterval> previous;unsigned tiles=0;
        const auto summary=CheckpointRun::bsgs(worker.journal(),grant,targets,table,verifier,[&](const auto& batch){
            const auto& tile=batch.interval();bool inside=false;
            for(const auto& gap:before.remaining)inside|=gap.contains(tile);
            require(inside,"tile crossed previously accepted coverage");
            if(!previous||tile.begin()!=previous->begin()||tile.end()!=previous->end()){
                if(previous)require(reverse?tile.end()<=previous->begin():tile.begin()>=previous->end(),"gap traversal changed direction");
                previous=tile;++tiles;
            }
            return execute(batch,targets,verifier,1024);
        },options);
        require(summary.complete&&summary.resumed_scalars==UInt256(19)&&summary.computed_scalars==UInt256(38)&&tiles==7,"fragmented complement was not exact");
        require(worker.journal().results(grant.scope).size()==5,"recovered worker recomputed old owner's target");
        // The prior owner's result stays server-side. A lost reply must replay
        // the exact pending upload and preserve its union with the new results.
        Json pending;rejects([&]{worker.synchronize([&](const Json& sent)->Json{
            pending=sent;transport(sent);throw std::runtime_error("lost upload reply");
        },true);});
        const auto rows=repo.request(cert,"GET",path+"/results");
        std::set<UInt256> expected{UInt256(101),UInt256(102),UInt256(120),UInt256(137),UInt256(156),UInt256(157)};
        require(rows.size()==expected.size(),"server lost result union");
        for(const auto& row:rows)require(expected.erase(UInt256::from_hex(row["scalar"].get<std::string>())),"unexpected result");
        worker.synchronize([&](const Json& sent){require(sent==pending,"pending upload changed");return transport(sent);},true);
        require(worker.status()["outbox_bytes"]==0&&!worker.next("gpu"),"finished grant dispatched again");
        require(repo.request(cert,"GET",path+"/results")==rows,"retry duplicated results");
        worker.journal().check();repo.admin({{"operation","check"}});
    }
    std::cout<<"BSGS fragmented grant recovery in both directions and upload retry passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
