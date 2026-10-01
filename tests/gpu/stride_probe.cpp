// The same checked scalar mapping is tested as host C++ and as native GPU code.
#include "common/scalar_stride.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef KEYHUNT_GPU_PROBE
#include "runtime.h"
#endif
struct Request { unsigned operation=0,length=0; uint8_t bytes[144]{}; };
struct Result { uint8_t scalar[32]{}; unsigned valid=0; };
KEYHUNT_HD Result evaluate(const Request& request) {
    Result result;
    using namespace keyhunt::gpu;
    if(request.length!=72)return result;
    const auto begin=scalar_from_bytes(request.bytes),stride=scalar_from_bytes(request.bytes+32);
    uint64_t offset=0;for(unsigned i=64;i<72;++i)offset=(offset<<8)|request.bytes[i];
    Scalar scalar;
    const bool valid=request.operation==1?reverse_scalar(begin,stride,offset,scalar):stride_scalar(begin,stride,offset,scalar);
    if(!valid)return result;
    result.valid=1;
    for(unsigned i=0;i<32;++i)result.scalar[i]=uint8_t(scalar.limb[(31-i)/4]>>(8*((31-i)%4)));
    return result;
}
Request parse(const std::string& line) {
    if(line.size()<2||line[1]!=' '||line[0]<'0'||line[0]>'1'||(line.size()-2)%2||line.size()>290)
        throw std::invalid_argument("expected operation 0 or 1 and at most 144 hex bytes");
    Request request;request.operation=unsigned(line[0]-'0');request.length=(line.size()-2)/2;
    auto digit=[](char c)->unsigned { const auto index=std::string("0123456789abcdef").find(c);
        if(index==std::string::npos)throw std::invalid_argument("invalid hex");
        return unsigned(index); };
    for(unsigned i=0;i<request.length;++i)request.bytes[i]=uint8_t(16*digit(line[2+2*i])+digit(line[3+2*i]));
    return request;
}
#ifdef KEYHUNT_GPU_PROBE
using keyhunt::backend::gpu_check;
__global__ void stride_kernel(const Request* input,Result* output,unsigned count,unsigned long long* completed) {
    const unsigned index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index>=count)return;
    output[index]=evaluate(input[index]);atomicAdd(completed,1ULL);
}
template<class T> struct Buffer {
    T* data=nullptr;
    explicit Buffer(size_t count){gpu_check(gpuMalloc(&data,count*sizeof(T)),"allocate stride probe");}
    ~Buffer(){if(data)(void)gpuFree(data);}
};
void run(const std::vector<Request>& requests,std::vector<Result>& results) {
    const unsigned count=unsigned(requests.size());
    Buffer<Request> input(count);Buffer<Result> output(count+1);Buffer<unsigned long long> completed(1);
    gpu_check(gpuMemcpy(input.data,requests.data(),count*sizeof(Request),gpuMemcpyHostToDevice),"copy stride requests");
    gpu_check(gpuMemset(output.data,0xa5,(count+1)*sizeof(Result)),"initialize stride guard");
    gpu_check(gpuMemset(completed.data,0,sizeof(unsigned long long)),"initialize stride count");
    (void)gpuGetLastError();
    gpuLaunchKernelGGL(stride_kernel,dim3((count+127)/128),dim3(128),0,0,input.data,output.data,count,completed.data);
    gpu_check(gpuGetLastError(),"launch stride probe");
    // Blocking copies wait for this probe's default-stream launch.
    results.resize(count+1);
    gpu_check(gpuMemcpy(results.data(),output.data,(count+1)*sizeof(Result),gpuMemcpyDeviceToHost),"copy stride results");
    unsigned long long executed=0;
    gpu_check(gpuMemcpy(&executed,completed.data,sizeof(executed),gpuMemcpyDeviceToHost),"copy stride count");
    const auto* guard=reinterpret_cast<const uint8_t*>(&results[count]);
    if(executed!=count||!std::all_of(guard,guard+sizeof(Result),[](uint8_t b){return b==0xa5;}))
        throw std::runtime_error("stride probe count or tail guard mismatch");
    results.resize(count);
}
#else
void run(const std::vector<Request>& requests,std::vector<Result>& results) {
    for(const auto& request:requests)results.push_back(evaluate(request));
}
#endif
int main(){try{
    std::vector<Request> requests;
    for(std::string line;std::getline(std::cin,line);){
        if(requests.size()==4096)throw std::invalid_argument("probe request limit exceeded");
        requests.push_back(parse(line));
    }
    if(requests.empty())return 0;
    std::vector<Result> results;run(requests,results);
    for(size_t i=0;i<requests.size();++i){
        if(!results[i].valid){std::cout<<"invalid\n";continue;}
        const char digits[]="0123456789abcdef";for(auto byte:results[i].scalar)std::cout<<digits[byte>>4]<<digits[byte&15];
        std::cout<<'\n';
    }
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
