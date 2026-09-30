#include "fixture.h"
#include <iostream>
using namespace cfixture;
int main(){
    try{
        Temporary temp;int64_t now=1800000000;Repository repo(temp.path.string(),[&]{return now;});
        const auto first=pem(1,now-60,now+86400),second=pem(2,now-60,now+86400),rotation=pem(3,now-60,now+86400);
        const auto a=certificate(first),b=certificate(second),rotated=certificate(rotation);
        require(a.fingerprint!=b.fingerprint,"same CN conflated certificates");
        require(unbase64(base64(first))==first,"forwarded certificate roundtrip");
        denied(401,[&]{unbase64("not a certificate");});
        denied(401,[&]{certificate(first+second);});
        denied(401,[&]{certificate(pem(4,now-60,now+86400,false));});
        denied(400,[&]{parse_json("{\"a\":1,\"a\":2}");});
        denied(401,[&]{repo.request(a,"GET","/api/v1/projects");});
        denied(409,[&]{repo.admin({{"operation","client-add"},{"name","worker"},{"certificate",first}});});
        const auto alice=repo.admin({{"operation","bootstrap"},{"name","owner"},{"certificate",first}});
        denied(409,[&]{repo.admin({{"operation","bootstrap"},{"name","again"},{"certificate",second}});});
        const auto bob=repo.admin({{"operation","client-add"},{"name","worker"},{"certificate",second}});
        const auto project=repo.admin({{"operation","project-create"},{"name","first"},{"owner",alice["client"]}})["project"].get<std::string>();
        const auto other=repo.admin({{"operation","project-create"},{"name","second"},{"owner",bob["client"]}})["project"].get<std::string>();
        core::XPointVerifier verifier;const auto input=job_input(x_targets(verifier,{1,50,100}));
        const auto created=repo.request(a,"POST","/api/v1/projects/"+project+"/jobs",input);
        const auto duplicate=repo.request(b,"POST","/api/v1/projects/"+other+"/jobs",input);
        require(created["job"]==duplicate["job"],"identical manifests changed identity across projects");
        const std::string path="/api/v1/projects/"+project+"/jobs/"+created["job"].get<std::string>();
        require(repo.request(a,"GET","/api/v1/projects").size()==1,"project list leaked membership");
        for(const auto* route:{"/status","/blocks/0x0000000000000000000000000000000000000000000000000000000000000000","/results","/results/0/1"})
            denied(404,[&]{repo.request(b,"GET",path+route);});
        for(const auto* route:{"/pause","/blocks/0x0000000000000000000000000000000000000000000000000000000000000000/recover"})
            denied(404,[&]{repo.request(b,"POST",path+route,Json::object());});
        denied(404,[&]{repo.request(b,"POST","/api/v1/projects/"+project+"/jobs",input);});
        denied(404,[&]{repo.request(b,"POST","/api/v1/projects/"+project+"/memberships",Json::object());});
        denied(404,[&]{repo.request(b,"GET","/api/v1/projects/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa/jobs/"+created["job"].get<std::string>()+"/status");});
        repo.admin({{"operation","membership-set"},{"project",project},{"client",bob["client"]},{"role","reader"}});
        require(repo.request(b,"GET",path+"/status")["unexplored"]==UInt256(10).hex(),"reader status");
        denied(403,[&]{repo.request(b,"POST",path+"/pause",{{"paused",true}});});
        denied(403,[&]{repo.request(b,"GET",path+"/results");});
        denied(403,[&]{repo.request(b,"POST","/api/v1/projects/"+project+"/jobs",input);});
        repo.request(a,"POST","/api/v1/projects/"+project+"/memberships",{{"client",bob["client"]},{"role","worker"}});
        denied(403,[&]{repo.request(b,"POST","/api/v1/projects/"+project+"/memberships",{{"client",bob["client"]},{"role","owner"}});});
        denied(403,[&]{repo.request(b,"POST",path+"/blocks/"+UInt256().hex()+"/recover",Json::object());});
        denied(403,[&]{repo.request(b,"GET",path+"/results/0/1");});
        require(repo.request(a,"POST",path+"/pause",{{"paused",true}})["paused"]==true,"owner pause");
        // Reauthorization happens on every call, even with the same parsed cert
        // object (the service can receive these on one persistent TLS connection).
        repo.admin({{"operation","credential-set"},{"fingerprint",bob["fingerprint"]},{"enabled",false}});
        denied(401,[&]{repo.request(b,"GET",path+"/status");});
        repo.admin({{"operation","credential-add"},{"client",bob["client"]},{"certificate",rotation}});
        require(repo.request(rotated,"GET",path+"/status")["paused"]==true,"rotation lost stable client roles");
        denied(401,[&]{repo.request(b,"GET",path+"/status");});
        repo.admin({{"operation","client-set"},{"client",bob["client"]},{"enabled",false}});
        denied(401,[&]{repo.request(rotated,"GET",path+"/status");});
        repo.admin({{"operation","client-set"},{"client",bob["client"]},{"enabled",true}});
        now+=86400;denied(401,[&]{repo.request(rotated,"GET",path+"/status");});now-=86400;
        denied(400,[&]{repo.admin({{"operation","client-add"},{"name","expired"},{"certificate",pem(5,now-100,now-1)}});});
        repo.admin({{"operation","membership-set"},{"project",project},{"client",bob["client"]},{"role","none"}});
        denied(404,[&]{repo.request(rotated,"GET",path+"/status");});
        // Failed nested project/role creation rolls back the project as well.
        const auto before=repo.request(a,"GET","/api/v1/projects").size();
        denied(404,[&]{repo.admin({{"operation","project-create"},{"name","rollback"},{"owner","aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"}});});
        require(repo.request(a,"GET","/api/v1/projects").size()==before,"failed admin mutation leaked a project");
        repo.admin({{"operation","check"}});
        std::cout<<"Enrollment, certificate identity, project isolation, roles, rotation and revocation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
