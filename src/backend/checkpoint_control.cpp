#include "checkpoint_control.h"
#include "state_helpers.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace keyhunt::backend {
namespace {
using namespace storage;
using Clock=std::chrono::steady_clock;
// HIP may already have background threads when handlers are installed. A
// current-thread signal mask cannot protect a read/clear from a handler on
// another thread. Lock-free exchange consumes flags without losing that race.
static_assert(std::atomic<sig_atomic_t>::is_always_lock_free,"signal flags must never take a lock");
std::atomic<sig_atomic_t> signal_command{0},signal_stops{0},force_signal{0};
bool signals_owned=false;
constexpr std::array<int,4> control_signals{SIGINT,SIGTERM,SIGUSR1,SIGUSR2};

void handler(int signal){
    // No SQL, allocation, output, GPU call or process teardown in a handler.
    if(signal==SIGINT || signal==SIGTERM){
        if(signal_stops.exchange(1,std::memory_order_relaxed))
            force_signal.store(signal,std::memory_order_relaxed);
    }else signal_command.store(signal==SIGUSR1?1:2,std::memory_order_relaxed);
}
struct Signals {
    std::array<struct sigaction,4> previous{};
    size_t installed=0;
    Signals(){
        if(signals_owned)throw std::logic_error("checkpoint signal owner already exists");
        signal_command.store(0,std::memory_order_relaxed);
        signal_stops.store(0,std::memory_order_relaxed);
        force_signal.store(0,std::memory_order_relaxed);
        struct sigaction action{};action.sa_handler=handler;sigemptyset(&action.sa_mask);
        for(int s:control_signals)sigaddset(&action.sa_mask,s);
        for(int s:control_signals){
            if(sigaction(s,&action,&previous[installed])){
                while(installed){--installed;sigaction(control_signals[installed],&previous[installed],nullptr);}
                throw std::runtime_error("cannot install checkpoint signal handlers");
            }
            ++installed;
        }
        signals_owned=true;
    }
    ~Signals(){
        while(installed){--installed;sigaction(control_signals[installed],&previous[installed],nullptr);}
        signals_owned=false;
    }
};
struct Fd {
    int value=-1;
    explicit Fd(int v=-1):value(v){}
    ~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;
};
std::string socket_path(const std::string& directory,const std::string& slot){
    if(slot.size()>128)throw std::invalid_argument("control slot is too long");
    for(unsigned char c:slot)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='@'))
        throw std::invalid_argument("invalid control slot");
    return directory+(slot.empty()?"/control.sock":"/control-"+slot+".sock");
}
sockaddr_un address(const std::string& directory,const std::string& slot){
    sockaddr_un out{};out.sun_family=AF_UNIX;const auto path=socket_path(directory,slot);
    if(path.size()>=sizeof(out.sun_path))throw std::invalid_argument("checkpoint control socket path is too long");
    std::memcpy(out.sun_path,path.c_str(),path.size()+1);return out;
}
bool same_user(int fd){
    ucred peer{};socklen_t size=sizeof(peer);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)==0 && size==sizeof(peer) && peer.uid==geteuid();
}
const char* request_name(CheckpointRequest request){
    return request==CheckpointRequest::Run?"run":request==CheckpointRequest::Pause?"pause":"stop";
}
}

struct LocalCheckpointControl::Impl {
    std::string directory,path,identity,base_identity,slot;
    Signals signals;
    Fd listener;
    struct Client {int fd;Clock::time_point opened;};
    std::vector<Client> clients;
    CheckpointRequest requested=CheckpointRequest::Run;
    std::string activity="running";
    Clock::time_point pause_start{};
    double pause_ms=0;
    bool published=false;
    dev_t socket_device{};ino_t socket_inode{};

    Impl(const std::string& dir,const Grant* grant,int device,size_t visible,const std::string& name)
        :directory(dir),path(socket_path(dir,name)),slot(name){
        base_identity=",\"pid\":"+std::to_string(getpid())+",\"device\":"+std::to_string(device)+
            ",\"visible_devices\":"+std::to_string(visible);
        bind(grant);
    }
    void bind(const Grant* grant){
        using namespace state_detail;
        identity=base_identity;
        if(grant)identity+=",\"project\":"+quote(grant->scope.project)+
            ",\"job\":"+quote(hex(grant->scope.job.data(),32))+",\"block\":"+quote(grant->block.hex())+
            ",\"assignment_generation\":"+quote(std::to_string(grant->generation));
    }
    ~Impl(){close();}
    void close() noexcept{
        for(const auto& c:clients)::close(c.fd);
        clients.clear();
        if(listener.value>=0){::close(listener.value);listener.value=-1;}
        // Never unlink a replacement endpoint or an unexpected file.
        struct stat st{};
        if(published && lstat(path.c_str(),&st)==0 && st.st_dev==socket_device && st.st_ino==socket_inode)::unlink(path.c_str());
        published=false;
    }
    void open(){
        if(listener.value>=0)return;
        const auto addr=address(directory,slot);
        struct stat st{};
        if(lstat(path.c_str(),&st)==0){
            // executor.lock is held: any prior socket is an abandoned endpoint.
            // Reject symlinks/regular files instead of removing user data.
            if(!S_ISSOCK(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077))
                throw std::runtime_error("unsafe checkpoint control socket");
            if(::unlink(path.c_str()))throw std::runtime_error("cannot remove stale checkpoint control socket");
        }else if(errno!=ENOENT)throw std::runtime_error("cannot inspect checkpoint control socket");
        listener.value=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(listener.value<0 || ::bind(listener.value,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)))
            throw std::runtime_error("cannot bind checkpoint control socket");
        // The containing journal directory is already private (0700).
        if(lstat(path.c_str(),&st))throw std::runtime_error("cannot inspect new checkpoint control socket");
        socket_device=st.st_dev;socket_inode=st.st_ino;published=true;
        if(chmod(path.c_str(),0600) || listen(listener.value,16))throw std::runtime_error("cannot listen on checkpoint control socket");
    }
    std::string status(bool accepted=true)const{
        return "{\"type\":\"control\",\"accepted\":"+std::string(accepted?"true":"false")+
            ",\"state\":\""+activity+"\",\"requested\":\""+request_name(requested)+
            "\",\"durably_paused\":"+(activity=="paused"?"true":"false")+
            ",\"durability\":\"local\",\"pause_ms\":"+std::to_string(pause_ms)+identity+"}";
    }
    bool change(char command){
        if(command=='?')return true;
        if(command=='R'){
            if(requested==CheckpointRequest::Stop)return false;
            if(activity=="running")return true; // idempotent resume
            if(activity!="paused" && activity!="resuming" && activity!="idle" && activity!="completed")return false;
            requested=CheckpointRequest::Run;activity="resuming";return true;
        }
        if(command!='P' && command!='T')return false;
        if(command=='P' && activity=="resuming")return false; // finish resume validation first
        if(requested==CheckpointRequest::Stop)return command=='T';
        if(command=='P' && requested==CheckpointRequest::Pause)return true;
        requested=command=='P'?CheckpointRequest::Pause:CheckpointRequest::Stop;
        if(activity!="paused" && activity!="draining"){
            pause_start=Clock::now();pause_ms=0;
        }
        if(command=='T' || activity!="paused")activity="draining";
        return true;
    }
    CheckpointRequest poll(){
        // Force exit is deliberately outside the handler. It is observed at the
        // next bounded owner boundary; a hung driver still requires SIGKILL.
        const auto forced=force_signal.load(std::memory_order_relaxed);
        if(forced)::_exit(128+forced);
        if(signal_stops.load(std::memory_order_relaxed))change('T');
        // Keep the normal hot path read-only; exchange only a pending request.
        if(signal_command.load(std::memory_order_relaxed)){
            const auto command=signal_command.exchange(0,std::memory_order_relaxed);
            if(command)change(command==1?'P':'R');
        }
        open();
        // Fixed client/batch limits keep a stalled or noisy local client from
        // holding up checkpoints or growing memory without bound.
        for(unsigned n=0;n<16 && clients.size()<16;++n){
            int fd=accept4(listener.value,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd<0){
                if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)break;
                throw std::runtime_error("checkpoint control accept failed");
            }
            if(!same_user(fd)){::close(fd);continue;}
            clients.push_back({fd,Clock::now()});
        }
        for(auto it=clients.begin();it!=clients.end();){
            char data[2]{};
            const auto size=recv(it->fd,data,sizeof(data),MSG_DONTWAIT|MSG_TRUNC);
            if(size<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) &&
               Clock::now()-it->opened<std::chrono::seconds(2)){++it;continue;}
            if(size>0){
                const auto answer=status(size==1 && change(data[0]));
                // A vanished/full client cannot turn a successful checkpoint into
                // a process-wide SIGPIPE or a blocking write.
                send(it->fd,answer.data(),answer.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
            }
            ::close(it->fd);it=clients.erase(it);
        }
        return requested;
    }
    void notify(CheckpointActivity value){
        switch(value){
        case CheckpointActivity::Idle:activity="idle";break;
        case CheckpointActivity::Draining:activity="draining";break;
        case CheckpointActivity::Paused:
            activity="paused";pause_ms=std::chrono::duration<double,std::milli>(Clock::now()-pause_start).count();break;
        case CheckpointActivity::Running:activity="running";break;
        case CheckpointActivity::Stopped:activity="stopped";break;
        case CheckpointActivity::Completed:activity="completed";break;
        }
        std::cout<<status()<<'\n'<<std::flush;
        if(!std::cout)throw std::runtime_error("checkpoint control output failed; committed state is retained");
    }
};
LocalCheckpointControl::LocalCheckpointControl(const std::string& dir,const Grant& grant,int device,size_t visible)
    :impl_(std::make_unique<Impl>(dir,&grant,device,visible,"")){}
LocalCheckpointControl::LocalCheckpointControl(const std::string& dir,int device,size_t visible,const std::string& slot)
    :impl_(std::make_unique<Impl>(dir,nullptr,device,visible,slot)){}
void LocalCheckpointControl::bind(const Grant* grant){impl_->bind(grant);}
LocalCheckpointControl::~LocalCheckpointControl()=default;
CheckpointControl LocalCheckpointControl::callbacks(){
    return {[this]{return impl_->poll();},[this](auto a){impl_->notify(a);},
        []{std::this_thread::sleep_for(std::chrono::milliseconds(20));}};
}
void LocalCheckpointControl::close() noexcept{impl_->close();}
std::string LocalCheckpointControl::command(const std::string& directory,const std::string& action,const std::string& slot){
    const char code=action=="pause"?'P':action=="resume"?'R':action=="stop"?'T':'?';
    const auto addr=address(directory,slot);
    Fd fd(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));
    if(fd.value<0)throw std::runtime_error("cannot create checkpoint control client");
    timeval timeout{5,0};
    if(setsockopt(fd.value,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
       setsockopt(fd.value,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)))
        throw std::runtime_error("cannot set checkpoint control timeout");
    if(connect(fd.value,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)) || !same_user(fd.value))
        throw std::runtime_error("no reachable local checkpoint owner; inspect durable state with state block/check");
    if(send(fd.value,&code,1,MSG_NOSIGNAL)!=1)throw std::runtime_error("checkpoint control send failed");
    char response[2048]{};
    const auto size=recv(fd.value,response,sizeof(response),MSG_TRUNC);
    if(size<=0 || size>=ssize_t(sizeof(response)))
        throw std::runtime_error("checkpoint control reply unavailable; request may be pending, inspect status before retrying");
    return std::string(response,size);
}
} // namespace keyhunt::backend
