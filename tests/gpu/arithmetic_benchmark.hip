// Opt-in microbenchmark. It emits checked results and raw timing samples, not a
// key-search rate. No clocks, partition modes or application tuning are changed.
#include "device_runtime.h"
#include "common/point.h"
#include "multiply16.h"
#include "runtime.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>
using namespace keyhunt::gpu;
using keyhunt::backend::gpu_check;
struct Input { Field a,b; Point point; };
struct Output { Field field; Point point; };

template<unsigned Kind>
KEYHUNT_HD Output compute(const Input& input, unsigned rounds) {
    Output result{};
    if constexpr (Kind < 3) {
        result.field = input.a;
        for (unsigned i = 0; i < rounds; ++i) {
            if constexpr (Kind == 0) mul(result.field,result.field,input.b);
            if constexpr (Kind == 1) keyhunt::gpu::test::multiply16(result.field,result.field,input.b);
            if constexpr (Kind == 2) inverse(result.field,result.field);
        }
    } else {
        result.point = input.point;
        for (unsigned i = 0; i < rounds; ++i) point_double(result.point,result.point);
    }
    return result;
}
template<unsigned Kind>
__global__ void benchmark_kernel(const Input* input, Output* output, uint64_t count, unsigned rounds) {
    const uint64_t index = uint64_t(blockIdx.x)*blockDim.x + threadIdx.x;
    if (index < count) output[index] = compute<Kind>(input[index],rounds);
}
template<class T> struct Buffer {
    T* data = nullptr;
    explicit Buffer(size_t count) { gpu_check(gpuMalloc(&data,count*sizeof(T)),"benchmark allocation"); }
    ~Buffer() { if (data) (void)gpuFree(data); }
};
struct Event {
    gpuEvent_t value = nullptr;
    Event() { gpu_check(gpuEventCreate(&value),"benchmark event creation"); }
    ~Event() { if (value) (void)gpuEventDestroy(value); }
};
int main(int argc, char** argv) {
    try {
        const int device = argc == 1 ? 0 : std::stoi(argv[1]);
        if (argc > 2) throw std::invalid_argument("usage: hip_arithmetic_benchmark [device]");
        keyhunt::backend::DeviceScope selected(device);
        constexpr unsigned count = 1025; // partial final workgroup remains covered
        const unsigned rounds[4]{32,32,1,16};
        const char* names[4]{"multiply_8x32","multiply_16x16","inverse","point_double"};
        std::mt19937_64 random(0xC08BEEF);
        std::vector<Input> input(count);
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned limb = 0; limb < 8; ++limb) {
                input[i].a.limb[limb] = uint32_t(random()); input[i].b.limb[limb] = uint32_t(random());
            }
            input[i].a = normalize(input[i].a); input[i].b = normalize(input[i].b);
            Scalar scalar{}; scalar.limb[0] = i+1;
            point_multiply(input[i].point,generator(),scalar);
        }
        std::vector<Output> expected[4];
        for (auto& values : expected) values.reserve(count);
        for (const auto& value : input) {
            expected[0].push_back(compute<0>(value,rounds[0]));
            // The candidate must agree with the 8x32 path, not with itself.
            expected[1].push_back(expected[0].back());
            expected[2].push_back(compute<2>(value,rounds[2]));
            expected[3].push_back(compute<3>(value,rounds[3]));
        }
        Buffer<Input> device_input(count);
        Buffer<Output> device_output(count+1);
        Event start,done;
        gpu_check(gpuMemcpy(device_input.data,input.data(),count*sizeof(Input),gpuMemcpyHostToDevice),"benchmark upload");
        std::vector<Output> output(count+1);
        std::vector<float> times[4]; std::vector<double> walls[4];
        for (int sample = -1; sample < 5; ++sample) {
            for (unsigned order = 0; order < 4; ++order) {
                const unsigned kind = sample >= 0 && sample%2 ? 3-order : order;
                const auto wall_start = std::chrono::steady_clock::now();
                gpu_check(gpuMemset(device_output.data,0xa5,output.size()*sizeof(Output)),"benchmark output guard");
                gpu_check(gpuEventRecord(start.value),"benchmark start");
                (void)gpuGetLastError();
                switch (kind) {
                case 0: gpuLaunchKernelGGL(benchmark_kernel<0>,dim3((count+127)/128),dim3(128),0,0,device_input.data,device_output.data,count,rounds[kind]); break;
                case 1: gpuLaunchKernelGGL(benchmark_kernel<1>,dim3((count+127)/128),dim3(128),0,0,device_input.data,device_output.data,count,rounds[kind]); break;
                case 2: gpuLaunchKernelGGL(benchmark_kernel<2>,dim3((count+127)/128),dim3(128),0,0,device_input.data,device_output.data,count,rounds[kind]); break;
                case 3: gpuLaunchKernelGGL(benchmark_kernel<3>,dim3((count+127)/128),dim3(128),0,0,device_input.data,device_output.data,count,rounds[kind]); break;
                }
                gpu_check(gpuGetLastError(),"benchmark kernel launch");
                gpu_check(gpuEventRecord(done.value),"benchmark end");
                gpu_check(gpuEventSynchronize(done.value),"benchmark completion");
                float elapsed = 0;
                gpu_check(gpuEventElapsedTime(&elapsed,start.value,done.value),"benchmark timing");
                gpu_check(gpuMemcpy(output.data(),device_output.data,output.size()*sizeof(Output),gpuMemcpyDeviceToHost),"benchmark download");
                for (unsigned i = 0; i < count; ++i) {
                    const auto& want = expected[kind][i]; const auto& have = output[i];
                    if (!equal(want.field,have.field) || !equal(want.point.x,have.point.x) ||
                        !equal(want.point.y,have.point.y) || !equal(want.point.z,have.point.z))
                        throw std::runtime_error("benchmark output mismatch");
                }
                const auto* guard = reinterpret_cast<const unsigned char*>(&output[count]);
                if (!std::all_of(guard,guard+sizeof(Output),[](unsigned char c){return c==0xa5;}))
                    throw std::runtime_error("benchmark tail guard overwritten");
                if (sample >= 0) {
                    times[kind].push_back(elapsed);
                    walls[kind].push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall_start).count());
                }
            }
        }
        std::cout << "{\"device\":" << device << ",\"seed\":201899759,\"items\":" << count << ",\"warmup_per_kernel\":1,\"measured_samples\":5,\"kernels\":[";
        for (unsigned kind = 0; kind < 4; ++kind) {
            if (kind) std::cout << ',';
            std::cout << "{\"name\":\"" << names[kind] << "\",\"rounds\":" << rounds[kind] << ",\"kernel_ms\":[";
            for (unsigned i=0;i<5;++i) std::cout << (i?",":"") << times[kind][i];
            std::cout << "],\"wall_ms\":[";
            for (unsigned i=0;i<5;++i) std::cout << (i?",":"") << walls[kind][i];
            std::cout << "]}";
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
