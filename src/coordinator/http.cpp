#include "http.h"
#include "protocol.h"
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace keyhunt::coordination {
namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t max_headers=32768,max_body=8*1024*1024;
std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free,"signal flag must be lock-free");
void stop(int){stopped.store(true,std::memory_order_relaxed);}
struct Fd{
    int fd=-1;explicit Fd(int value=-1):fd(value){}~Fd(){if(fd>=0)::close(fd);}Fd(const Fd&)=delete;
};
sockaddr_un address(const std::string& path){
    sockaddr_un out{};out.sun_family=AF_UNIX;
    if(path.empty()||path[0]!='/'||path.size()>=sizeof(out.sun_path)||path.find('\0')!=std::string::npos)
        throw std::invalid_argument("Unix socket requires an absolute path of at most 107 bytes");
    std::memcpy(out.sun_path,path.c_str(),path.size()+1);return out;
}
uid_t peer(int fd){
    ucred value{};socklen_t n=sizeof(value);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&value,&n)||n!=sizeof(value))throw Error(403,"untrusted local peer");
    return value.uid;
}
void ready(int fd,short events,Clock::time_point deadline){
    for(;;){
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<=0)throw Error(408,"request deadline exceeded");
        pollfd wait{fd,events,0};const int n=poll(&wait,1,int(remaining));
        if(n<0&&errno==EINTR)continue;
        if(n<=0)throw Error(408,"request deadline exceeded");
        if(wait.revents&events)return;
        throw Error(400,"connection closed");
    }
}
std::string lower(std::string s){for(char& c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return s;}
struct Message {std::string line,body;std::map<std::string,std::string> headers;};
Message read(int fd){
    // One request per backend connection, with an absolute deadline and strict
    // framing. Rejecting duplicate lengths/TE also avoids proxy desynchronization.
    const auto deadline=Clock::now()+std::chrono::seconds(15);std::string data;size_t end;
    while((end=data.find("\r\n\r\n"))==std::string::npos){
        if(data.size()>=max_headers)throw Error(431,"headers too large");
        ready(fd,POLLIN,deadline);char chunk[4096];auto n=recv(fd,chunk,sizeof(chunk),0);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<=0)throw Error(400,"short HTTP headers");
        data.append(chunk,size_t(n));
    }
    if(end>max_headers)throw Error(431,"headers too large");
    Message message;const auto first=data.find("\r\n");message.line=data.substr(0,first);
    if(first==std::string::npos||first>4096)throw Error(400,"invalid request line");
    for(size_t at=first+2;at<end;){
        const auto next=data.find("\r\n",at);const auto line=data.substr(at,next-at);at=next+2;
        const auto colon=line.find(':');if(colon==std::string::npos||colon==0)throw Error(400,"malformed header");
        const auto key=lower(line.substr(0,colon));for(unsigned char c:key)
            if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))throw Error(400,"invalid header name");
        auto value=line.substr(colon+1);while(!value.empty()&&value.front()==' ')value.erase(value.begin());
        while(!value.empty()&&value.back()==' ')value.pop_back();
        for(unsigned char c:value)if(c<32||c==127)throw Error(400,"invalid header value");
        if(!message.headers.emplace(key,value).second)throw Error(400,"duplicate HTTP header");
    }
    if(message.headers.count("transfer-encoding")||message.headers.count("expect"))throw Error(400,"unsupported HTTP framing");
    size_t size=0;
    if(message.headers.count("content-length")){
        const auto& value=message.headers.at("content-length");
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),size);
        if(value.empty()||parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())throw Error(400,"invalid content length");
    }
    if(size>max_body)throw Error(413,"body too large");
    message.body=data.substr(end+4);
    if(message.body.size()>size)throw Error(400,"unexpected pipelined or trailing bytes");
    while(message.body.size()<size){
        ready(fd,POLLIN,deadline);char chunk[8192];const auto n=recv(fd,chunk,std::min(sizeof(chunk),size-message.body.size()),0);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<=0)throw Error(400,"short HTTP body");
        message.body.append(chunk,size_t(n));
    }
    return message;
}
void send_all(int fd,const std::string& data){
    const auto deadline=Clock::now()+std::chrono::seconds(5);size_t sent=0;
    while(sent<data.size()){
        ready(fd,POLLOUT,deadline);const auto n=send(fd,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);
        if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(n<=0)throw Error(400,"response write failed");
        sent+=size_t(n);
    }
}
void reply(int fd,int status,const Json& value){
    const auto body=value.dump();if(body.size()>max_body)throw Error(413,"response too large");
    send_all(fd,"HTTP/1.1 "+std::to_string(status)+" Result\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body);
}
class Listener {
    std::string path;Fd socket_;dev_t device{};ino_t inode{};
public:
    Listener(const std::string& requested,mode_t mode):path(storage::detail::state_path(requested)){
        const auto parent=std::filesystem::path(path).parent_path();struct stat st{};
        if(lstat(parent.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0027))
            throw std::runtime_error("socket directory must be owned by the service user, without group write or other access");
        const auto addr=address(path);
        if(lstat(path.c_str(),&st)==0){
            if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid())throw std::runtime_error("unsafe existing socket");
            Fd probe(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
            if(probe.fd<0||connect(probe.fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))==0||errno!=ECONNREFUSED)
                throw std::runtime_error("socket already active or cannot safely replace it");
            if(unlink(path.c_str()))throw std::runtime_error("cannot remove stale socket");
        }else if(errno!=ENOENT)throw std::runtime_error("cannot inspect socket");
        socket_.fd=::socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(socket_.fd<0||bind(socket_.fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)))throw std::runtime_error("cannot bind Unix socket");
        if(lstat(path.c_str(),&st))throw std::runtime_error("cannot inspect bound socket");
        device=st.st_dev;inode=st.st_ino;
        if(chmod(path.c_str(),mode)||listen(socket_.fd,64))throw std::runtime_error("cannot listen on Unix socket");
    }
    ~Listener(){struct stat st{};if(lstat(path.c_str(),&st)==0&&st.st_dev==device&&st.st_ino==inode)unlink(path.c_str());}
    int fd()const{return socket_.fd;}
};
struct Rate {Clock::time_point since;unsigned requests=0;};
}
int serve(const ServerOptions& options){
    if(options.api_socket==options.admin_socket||options.authority.empty()||options.authority.size()>255||options.proxy_uid==uid_t(-1))
        throw std::invalid_argument("distinct sockets, authority and proxy UID are required");
    for(unsigned char c:options.authority)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c==':'))
        throw std::invalid_argument("authority must be a lowercase DNS name with optional port");
    Repository repository(options.state_directory);
    Fd lock(open((repository.directory()+"/coordinator.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600));
    struct stat st{};
    if(lock.fd<0||fstat(lock.fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1||flock(lock.fd,LOCK_EX|LOCK_NB))
        throw std::runtime_error("unsafe or already held coordinator lock");
    Listener api(options.api_socket,0660),admin(options.admin_socket,0600);
    const auto colon=options.authority.find(':');const auto hostname=options.authority.substr(0,colon);
    std::map<std::string,Rate> rates;
    stopped.store(false,std::memory_order_relaxed);struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,nullptr);sigaction(SIGTERM,&action,nullptr);
    std::cout<<Json({{"ready",true},{"api_socket",options.api_socket},{"admin_socket",options.admin_socket},{"authority",options.authority}}).dump()<<'\n'<<std::flush;
    while(!stopped.load(std::memory_order_relaxed)){
        // Administration has its own protected listener and cannot be reached
        // by choosing a remote route or forwarding an administrator header.
        pollfd listeners[]{{admin.fd(),POLLIN,0},{api.fd(),POLLIN,0}};
        const int count=poll(listeners,2,100);if(count<0&&errno==EINTR)continue;if(count<0)throw std::runtime_error("listener poll failed");
        for(unsigned i=0;i<2;++i){
            if(!(listeners[i].revents&POLLIN))continue;
            Fd client(accept4(listeners[i].fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC));if(client.fd<0)continue;
            int status=200;Json output;
            try{
                if(peer(client.fd)!=(i==0?geteuid():options.proxy_uid))throw Error(403,"untrusted local peer");
                const auto request=read(client.fd);std::istringstream line(request.line);std::string method,path,version,extra;
                if(!(line>>method>>path>>version)||(line>>extra)||version!="HTTP/1.1"||(method!="GET"&&method!="POST")||path.empty()||path[0]!='/')
                    throw Error(400,"unsupported request line");
                if(!request.headers.count("host")||(method=="GET"&&!request.body.empty())||
                   (method=="POST"&&(!request.headers.count("content-length")||request.headers.find("content-type")==request.headers.end()||request.headers.at("content-type")!="application/json")))
                    throw Error(400,"invalid request framing or content type");
                Json body=request.body.empty()?Json::object():parse_json(request.body),value,controls;
                if(i==0){
                    if(method!="POST"||path!="/admin")throw Error(404,"not found");
                    value=repository.admin(body);
                }else{
                    const std::set<std::string> identity_headers{"x-keyhunt-cert","x-keyhunt-tls-sni","x-keyhunt-tls-protocol","x-keyhunt-tls-verify"};
                    for(const auto& entry:request.headers)
                        if(entry.first.rfind("x-keyhunt-",0)==0&&!identity_headers.count(entry.first))throw Error(400,"unknown identity header");
                    auto header=[&](const char* name){auto it=request.headers.find(name);if(it==request.headers.end())throw Error(401,"missing verified TLS identity");return it->second;};
                    if(header("host")!=options.authority||header("x-keyhunt-tls-sni")!=hostname)throw Error(421,"authority mismatch");
                    if(header("x-keyhunt-tls-verify")!="SUCCESS")throw Error(401,"client certificate not verified");
                    const auto protocol=header("x-keyhunt-tls-protocol");if(protocol!="TLSv1.2"&&protocol!="TLSv1.3")throw Error(401,"unsupported TLS version");
                    if(request.headers.count("early-data"))throw Error(425,"early data is not accepted");
                    const auto cert=certificate(unbase64(header("x-keyhunt-cert")));
                    const auto now=Clock::now();for(auto it=rates.begin();it!=rates.end();)if(now-it->second.since>std::chrono::minutes(1))it=rates.erase(it);else ++it;
                    const auto key=wire::hex(cert.fingerprint);if(!rates.count(key)&&rates.size()>=4096)throw Error(429,"client rate table full");
                    auto& rate=rates.try_emplace(key,Rate{now,0}).first->second;
                    if(++rate.requests>120)throw Error(429,"client request limit exceeded");
                    value=repository.request(cert,method,path,body);
                    if(method=="POST"&&path=="/api/v1/sync")controls=repository.control_snapshot(cert,body);
                }
                output={{"ok",true},{"server_time",repository.now()},{"value",value}};
                // Current controls are transport metadata, like current time.
                // They must not mutate the exact cached assignment receipt.
                if(!controls.is_null())output["controls"]=controls;
            }catch(const Error& e){status=e.status;output={{"ok",false},{"error",e.what()}};}
            catch(const std::invalid_argument& e){status=400;output={{"ok",false},{"error",e.what()}};}
            catch(const std::exception& e){status=503;output={{"ok",false},{"error","coordinator state unavailable"}};std::cerr<<"coordinator: "<<e.what()<<'\n';}
            try{reply(client.fd,status,output);}catch(const std::exception&){}
        }
    }
    return 0;
}
Json local_admin(const std::string& path,const Json& body){
    const auto addr=address(path);Fd client(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
    if(client.fd<0||connect(client.fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))||peer(client.fd)!=geteuid())
        throw std::runtime_error("cannot connect to trusted local admin socket");
    if(fcntl(client.fd,F_SETFL,O_NONBLOCK)<0)throw std::runtime_error("cannot set admin socket nonblocking");
    const auto text=body.dump();
    send_all(client.fd,"POST /admin HTTP/1.1\r\nHost: local\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(text.size())+"\r\nConnection: close\r\n\r\n"+text);
    const auto response=read(client.fd);const auto result=parse_json(response.body);
    if(response.line.rfind("HTTP/1.1 200 ",0)!=0||!result.value("ok",false))throw std::runtime_error(result.value("error","admin request failed"));
    return result["value"];
}
} // namespace keyhunt::coordination
