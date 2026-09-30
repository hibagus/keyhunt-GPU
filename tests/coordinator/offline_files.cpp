#include "fixture.h"
#include "keyhunt/coordinator/offline.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace cfixture;
int main(){try{
    Temporary temporary;const auto file=(temporary.path/"request.json").string();
    const Json document{{"format","test"},{"version",1},{"payload",{{"scalar","1"}}}};
    const auto hash=publish_offline(file,document);
    require(hash==offline_checksum(document)&&read_offline(file,hash)==document,"file round trip");
    rejects([&]{publish_offline(file,Json{{"different",true}});});
    require(read_offline(file,hash)==document,"exclusive publication replaced the original");
    rejects([&]{read_offline(file,std::string(64,'0'));});
    rejects([&]{read_offline(file,"bad");});
    rejects([&]{publish_offline("relative.json",document);});
    const auto symbolic=(temporary.path/"symbolic").string();
    require(symlink(file.c_str(),symbolic.c_str())==0,"make symbolic fixture");
    rejects([&]{read_offline(symbolic,hash);});
    const auto linked=(temporary.path/"linked").string();require(link(file.c_str(),linked.c_str())==0,"make hardlink fixture");
    rejects([&]{read_offline(file,hash);});unlink(linked.c_str());
    chmod(file.c_str(),0644);rejects([&]{read_offline(file,hash);});chmod(file.c_str(),0600);
    const auto pipe=(temporary.path/"pipe").string();require(mkfifo(pipe.c_str(),0600)==0,"make pipe fixture");
    rejects([&]{read_offline(pipe,hash);}); // O_NONBLOCK prevents a hostile FIFO hang.
    const auto huge=(temporary.path/"huge").string();
    {std::ofstream out(huge);out.seekp(8*1024*1024);out.put('x');}chmod(huge.c_str(),0600);
    rejects([&]{read_offline(huge,hash);});
    rejects([&]{publish_offline((temporary.path/"big.json").string(),Json(std::string(8*1024*1024,'x')));});
    const auto malformed=(temporary.path/"malformed").string();
    const std::string duplicate="{\"a\":1,\"a\":2}\n";
    {std::ofstream out(malformed);out<<duplicate;}chmod(malformed.c_str(),0600);
    const auto duplicate_hash=wire::hex(storage::detail::digest({duplicate.begin(),duplicate.end()}));
    rejects([&]{read_offline(malformed,duplicate_hash);});
    chmod(temporary.path.c_str(),0755);rejects([&]{read_offline(file,hash);});
    rejects([&]{publish_offline((temporary.path/"public.json").string(),document);});chmod(temporary.path.c_str(),0700);
    for(const auto& entry:std::filesystem::directory_iterator(temporary.path))
        require(entry.path().filename().string().rfind(".offline-",0)!=0,"failed publication leaked staging file");
    std::cout<<"Offline checksums, private bounded reads and exclusive atomic publication passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
