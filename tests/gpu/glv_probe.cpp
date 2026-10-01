// Shared host/native probe. Expected signed scalars and points come from Python
// integers and pinned libsecp256k1, never from this adapter.
#include "common/glv.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef KEYHUNT_GPU_PROBE
#include "runtime.h"
#endif
using namespace keyhunt::gpu;
struct Request { unsigned operation=0; Scalar scalar{}; };
struct Result { unsigned valid=0; GlvSplit split{}; uint8_t point[64]{}; };
KEYHUNT_HD Result evaluate(const Request& request) {
    Result result;
    if(request.operation==0){result.valid=glv_split(result.split,request.scalar);return result;}
    Point point;
    if(request.operation==1){if(!public_key_glv(point,request.scalar))return result;}
    else{
        if(!public_key(point,request.scalar))return result;
        Point separate;point_endomorphism(separate,point);point_endomorphism(point,point);
        if(!equal(separate.x,point.x)||!equal(separate.y,point.y)||!equal(separate.z,point.z))return result;
    }
    const auto affine=to_affine(point);
    if(!on_curve(affine)||affine.infinity)return result;
    to_bytes(result.point,affine.x);to_bytes(result.point+32,affine.y);result.valid=1;return result;
}
Request parse(const std::string& line) {
    if(line.size()!=66||line[1]!=' '||line[0]<'0'||line[0]>'2')throw std::invalid_argument("expected operation 0..2 and 64 hex digits");
    Request request;request.operation=unsigned(line[0]-'0');uint8_t bytes[32];
    auto digit=[](char c){const auto d=std::string("0123456789abcdef").find(c);
        if(d==std::string::npos)throw std::invalid_argument("invalid hex digit");
        return unsigned(d);};
    for(unsigned i=0;i<32;++i)bytes[i]=uint8_t(16*digit(line[2+2*i])+digit(line[3+2*i]));
    request.scalar=scalar_from_bytes(bytes);return request;
}
#ifdef KEYHUNT_GPU_PROBE
using keyhunt::backend::gpu_check;
__global__ void glv_kernel(const Request* requests,Result* results,unsigned count,unsigned long long* completed) {
    const unsigned index=blockIdx.x*blockDim.x+threadIdx.x;
    if(index<count){results[index]=evaluate(requests[index]);atomicAdd(completed,1ULL);}
}
template<class T> struct Buffer {
    T* data=nullptr;explicit Buffer(size_t count){gpu_check(gpuMalloc(&data,count*sizeof(T)),"allocate GLV probe");}
    ~Buffer(){if(data)(void)gpuFree(data);}
};
void run(const std::vector<Request>& requests,std::vector<Result>& results) {
    const auto count=unsigned(requests.size());Buffer<Request> input(count);Buffer<Result> output(count+1);
    Buffer<unsigned long long> completed(1);
    gpu_check(gpuMemcpy(input.data,requests.data(),count*sizeof(Request),gpuMemcpyHostToDevice),"copy GLV requests");
    gpu_check(gpuMemset(output.data,0xa5,(count+1)*sizeof(Result)),"initialize GLV guard");
    gpu_check(gpuMemset(completed.data,0,sizeof(unsigned long long)),"initialize GLV count");
    (void)gpuGetLastError();
    gpuLaunchKernelGGL(glv_kernel,dim3((count+127)/128),dim3(128),0,0,input.data,output.data,count,completed.data);
    gpu_check(gpuGetLastError(),"launch GLV probe");results.resize(count+1);
    gpu_check(gpuMemcpy(results.data(),output.data,(count+1)*sizeof(Result),gpuMemcpyDeviceToHost),"copy GLV results");
    unsigned long long done=0;gpu_check(gpuMemcpy(&done,completed.data,sizeof(done),gpuMemcpyDeviceToHost),"copy GLV count");
    const auto* guard=reinterpret_cast<const uint8_t*>(&results[count]);
    if(done!=count||!std::all_of(guard,guard+sizeof(Result),[](uint8_t b){return b==0xa5;}))throw std::runtime_error("GLV count/guard mismatch");
    results.resize(count);
}
#else
void run(const std::vector<Request>& requests,std::vector<Result>& results){for(const auto& r:requests)results.push_back(evaluate(r));}
#endif
void hex(const uint8_t* bytes,unsigned count){const char* digits="0123456789abcdef";
    for(unsigned i=0;i<count;++i)std::cout<<digits[bytes[i]>>4]<<digits[bytes[i]&15];}
void component(const GlvSigned128& value){
    std::cout<<(value.negative?'-':'+');uint8_t bytes[16];
    for(unsigned i=0;i<16;++i)bytes[i]=uint8_t(value.limb[(15-i)/4]>>(8*((15-i)%4)));
    hex(bytes,16);
}
int main(){try{
    std::vector<Request> requests;
    for(std::string line;std::getline(std::cin,line);){if(requests.size()==8192)throw std::invalid_argument("GLV probe limit exceeded");requests.push_back(parse(line));}
    if(requests.empty())return 0;
    std::vector<Result> results;run(requests,results);
    for(size_t i=0;i<results.size();++i){const auto& result=results[i];
        if(!result.valid){std::cout<<"invalid\n";continue;}
        if(requests[i].operation==0){component(result.split.first);std::cout<<' ';component(result.split.second);}
        else{std::cout<<"04";hex(result.point,64);}std::cout<<'\n';
    }
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
