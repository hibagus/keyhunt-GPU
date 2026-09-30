#include "keyhunt/coordinator/offline.h"
#include "protocol.h"
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace keyhunt::coordination {
namespace {
constexpr size_t file_limit=8*1024*1024;
struct Descriptor {
    int value;
    explicit Descriptor(int fd):value(fd){if(fd<0)throw std::runtime_error("cannot open offline file or directory");}
    ~Descriptor(){close(value);}
    Descriptor(const Descriptor&)=delete;
    Descriptor& operator=(const Descriptor&)=delete;
};
std::string checksum(const std::string& text){
    return wire::hex(storage::detail::digest({text.begin(),text.end()}));
}
std::string encoded(const Json& document){
    const auto text=document.dump()+"\n";
    if(text.size()>file_limit)throw std::invalid_argument("offline file exceeds 8 MiB");
    return text;
}
struct Location {
    std::filesystem::path parent;
    std::string name;
    explicit Location(const std::string& path){
        const std::filesystem::path requested(path);
        if(!requested.is_absolute()||path.find('\0')!=std::string::npos||requested.filename().empty()||
           requested.filename()=="."||requested.filename()=="..")
            throw std::invalid_argument("offline file needs an absolute file path");
        // Reuse the journal's checkout exclusion, including enclosing Git roots.
        // Do not create or chmod a directory chosen by the operator.
        parent=storage::detail::state_path(requested.parent_path().string());
        name=requested.filename().string();
    }
};
void private_directory(int fd){
    struct stat info{};
    if(fstat(fd,&info)||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&0077))
        throw std::runtime_error("offline directory must be owned and private (0700)");
}
}
std::string offline_checksum(const Json& document){return checksum(encoded(document));}
std::string publish_offline(const std::string& path,const Json& document){
    const auto text=encoded(document);const Location location(path);
    Descriptor directory(open(location.parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    private_directory(directory.value);
    const auto temporary=".offline-"+storage::detail::uuid();
    Descriptor file(openat(directory.value,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    try{
        size_t done=0;
        while(done<text.size()){
            const auto count=write(file.value,text.data()+done,text.size()-done);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)throw std::runtime_error("offline file write failed");
            done+=size_t(count);
        }
        if(fsync(file.value))throw std::runtime_error("offline file sync failed");
        // Linux renameat2 publishes one complete inode without a transient second
        // hardlink and without overwriting any existing operator artifact.
        if(syscall(SYS_renameat2,directory.value,temporary.c_str(),directory.value,location.name.c_str(),RENAME_NOREPLACE))
            throw std::runtime_error("cannot publish offline file exclusively");
        if(fsync(directory.value))throw std::runtime_error("offline directory sync failed; destination may already exist");
    }catch(...){unlinkat(directory.value,temporary.c_str(),0);throw;}
    return checksum(text);
}
Json read_offline(const std::string& path,const std::string& expected){
    wire::digest(expected); // Require a full canonical checksum, never an optional hint.
    const Location location(path);
    Descriptor directory(open(location.parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    private_directory(directory.value);
    Descriptor file(openat(directory.value,location.name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
    struct stat info{};
    if(fstat(file.value,&info)||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||
       (info.st_mode&0077)||info.st_nlink!=1||info.st_size<0||uint64_t(info.st_size)>file_limit)
        throw std::runtime_error("offline file must be private, regular, singly linked and at most 8 MiB");
    std::string text;char buffer[16384];
    while(true){
        const auto count=read(file.value,buffer,sizeof(buffer));
        if(count<0&&errno==EINTR)continue;
        if(count<0)throw std::runtime_error("offline file read failed");
        if(!count)break;
        if(size_t(count)>file_limit-text.size())throw std::runtime_error("offline file exceeds 8 MiB");
        text.append(buffer,size_t(count));
    }
    if(checksum(text)!=expected)throw std::runtime_error("offline file checksum mismatch");
    return parse_json(text); // Also rejects ambiguous duplicate JSON fields.
}
void validate_offline_request(const Json& request){
    using namespace wire;
    fields(request,{"format","version","transfer","endpoint","body"});
    if(str(request,"format",64)!="keyhunt-offline-request"||integer(request,"version")!=1)
        throw Error(426,"unsupported offline request format");
    token(str(request,"transfer",36));
    const auto endpoint=str(request,"endpoint",512);
    if(endpoint.rfind("https://",0)!=0||endpoint.find_first_of("/?#@",8)!=std::string::npos)
        throw Error(400,"offline request requires an HTTPS authority");
    if(!request["body"].is_object())throw Error(400,"offline request body must be an object");
    offline_checksum(request); // The whole envelope must fit the portable file bound.
}
Json offline_response(const Json& request,int status,const Json& body){
    validate_offline_request(request);
    if(status!=200&&status!=401&&status!=403&&status!=404&&status!=409&&status!=426)
        throw Error(400,"not a definite offline acknowledgment or denial");
    Json response{{"format","keyhunt-offline-response"},{"version",1},
        {"transfer",request["transfer"]},{"request_sha256",offline_checksum(request)},
        {"status",status},{"body",body}};
    offline_checksum(response);
    return response;
}

}
