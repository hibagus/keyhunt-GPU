// Line-oriented synthetic arithmetic probe; compile once as C++ and once as HIP.
#include "arithmetic_cases.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#ifdef KEYHUNT_GPU_PROBE
#include "runtime.h"
#endif
using namespace keyhunt::gpu;
using namespace keyhunt::gpu::test;

Field decode(const std::string& text) {
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("expected 64 lowercase hexadecimal digits");
    Field value{};
    for (unsigned i = 0; i < 8; ++i)
        value.limb[i] = static_cast<uint32_t>(std::stoul(text.substr(56-8*i, 8), nullptr, 16));
    return value;
}
std::string encode(const Field& value) {
    uint8_t bytes[32]; to_bytes(bytes, value);
    std::string text;
    for (auto byte : bytes) { text += "0123456789abcdef"[byte>>4]; text += "0123456789abcdef"[byte&15]; }
    return text;
}
Request parse(const std::string& line) {
    Request request;
    std::istringstream words(line);
    std::string op, word;
    words >> op;
    if (op == "fnorm") request.op = Op::Normalize;
    else if (op == "fbytes") request.op = Op::Bytes;
    else if (op == "fadd") request.op = Op::Add;
    else if (op == "fsub") request.op = Op::Sub;
    else if (op == "fmul") request.op = Op::Mul;
    else if (op == "fsquare") request.op = Op::Square;
    else if (op == "fneg") request.op = Op::Negate;
    else if (op == "finv") request.op = Op::Inverse;
    else if (op == "finvgroup") request.op = Op::BatchInverse;
    else throw std::invalid_argument("unknown operation: " + op);
    while (words >> word) {
        if (request.count == 32) throw std::invalid_argument("group capacity exceeded");
        request.values[request.count++] = decode(word);
    }
    const bool binary = request.op == Op::Add || request.op == Op::Sub || request.op == Op::Mul;
    if (request.op != Op::BatchInverse && request.count != (binary ? 2U : 1U))
        throw std::invalid_argument("wrong operation arity");
    return request;
}
std::string format(const Request& request, const Result& result) {
    unsigned count = 1;
    std::string text;
    switch (request.op) {
    case Op::Add: case Op::Sub: case Op::Mul: count = 3; break;
    case Op::Square: case Op::Negate: count = 2; break;
    case Op::Inverse: case Op::Bytes: count = 2; text = std::to_string(result.flags); break;
    case Op::BatchInverse: count = request.count; text = std::to_string(result.flags); break;
    default: break;
    }
    for (unsigned i = 0; i < count; ++i) { if (!text.empty()) text += ' '; text += encode(result.values[i]); }
    return text;
}

#ifdef KEYHUNT_GPU_PROBE
using keyhunt::backend::hip_check;
__global__ void arithmetic_kernel(const Request* input, Result* output, uint64_t count,
                                  unsigned long long* executed) {
    const uint64_t index = uint64_t(blockIdx.x)*blockDim.x + threadIdx.x;
    if (index >= count) return;
    output[index] = evaluate(input[index]);
    atomicAdd(executed, 1ULL);
}
template<class T> struct Buffer {
    T* data = nullptr;
    explicit Buffer(size_t count) { hip_check(hipMalloc(&data, count*sizeof(T)), "hipMalloc(probe)"); }
    ~Buffer() { if (data) (void)hipFree(data); }
};
struct Event {
    hipEvent_t value = nullptr;
    Event() { hip_check(hipEventCreate(&value), "hipEventCreate(probe)"); }
    ~Event() { if (value) (void)hipEventDestroy(value); }
};
struct Runner {
    Buffer<Request> input;
    Buffer<Result> output;
    Buffer<unsigned long long> executed{1};
    Event start, done;
    double kernel_ms = 0;
    explicit Runner(size_t capacity) : input(capacity), output(capacity+1) {}
    void run(const std::vector<Request>& requests, std::vector<Result>& results) {
        const size_t count = requests.size();
        hip_check(hipMemcpy(input.data, requests.data(), count*sizeof(Request), hipMemcpyHostToDevice), "hipMemcpy(input)");
        hip_check(hipMemset(output.data, 0xa5, (count+1)*sizeof(Result)), "hipMemset(output)");
        hip_check(hipMemset(executed.data, 0, sizeof(unsigned long long)), "hipMemset(counter)");
        hip_check(hipEventRecord(start.value), "hipEventRecord(start)");
        (void)hipGetLastError();
        hipLaunchKernelGGL(arithmetic_kernel, dim3((count+127)/128), dim3(128), 0, 0,
                          input.data, output.data, count, executed.data);
        hip_check(hipGetLastError(), "arithmetic kernel launch");
        hip_check(hipEventRecord(done.value), "hipEventRecord(done)");
        hip_check(hipEventSynchronize(done.value), "hipEventSynchronize(probe)");
        float elapsed = 0;
        hip_check(hipEventElapsedTime(&elapsed, start.value, done.value), "hipEventElapsedTime(probe)");
        kernel_ms += elapsed;
        results.resize(count+1);
        hip_check(hipMemcpy(results.data(), output.data, results.size()*sizeof(Result), hipMemcpyDeviceToHost), "hipMemcpy(output)");
        unsigned long long completed = 0;
        hip_check(hipMemcpy(&completed, executed.data, sizeof(completed), hipMemcpyDeviceToHost), "hipMemcpy(counter)");
        if (completed != count) throw std::runtime_error("device arithmetic count mismatch");
        const auto* guard = reinterpret_cast<const unsigned char*>(&results[count]);
        if (!std::all_of(guard, guard+sizeof(Result), [](unsigned char byte) { return byte == 0xa5; }))
            throw std::runtime_error("partial workgroup overwrote arithmetic tail guard");
        results.resize(count);
    }
};
#else
struct Runner {
    double kernel_ms = 0;
    explicit Runner(size_t) {}
    void run(const std::vector<Request>& requests, std::vector<Result>& results) {
        results.clear();
        for (const auto& request : requests) results.push_back(evaluate(request));
    }
};
#endif
int main(int argc, char** argv) {
    try {
        unsigned batch = 257;
        int device = 0;
        std::string stats;
        for (int i = 1; i < argc; i += 2) {
            if (i+1 == argc) throw std::invalid_argument("missing option value");
            const std::string key = argv[i];
            if (key == "--batch") batch = std::stoul(argv[i+1]);
            else if (key == "--device") device = std::stoi(argv[i+1]);
            else if (key == "--stats") stats = argv[i+1];
            else throw std::invalid_argument("unknown probe option");
        }
        if (!batch || batch > 1024) throw std::invalid_argument("batch must be in [1,1024]");
#ifdef KEYHUNT_GPU_PROBE
        keyhunt::backend::DeviceScope selected(device);
#else
        (void)device;
#endif
        Runner runner(batch);
        std::vector<Request> requests;
        std::vector<Result> results;
        uint64_t count = 0, launches = 0;
        const auto start = std::chrono::steady_clock::now();
        auto flush = [&] {
            if (requests.empty()) return;
            runner.run(requests, results);
            for (size_t i = 0; i < requests.size(); ++i) std::cout << format(requests[i], results[i]) << '\n';
            count += requests.size(); ++launches; requests.clear();
        };
        for (std::string line; std::getline(std::cin, line);) {
            requests.push_back(parse(line));
            if (requests.size() == batch) flush();
        }
        flush();
        if (!stats.empty()) {
            const double wall = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::ofstream output(stats);
            output << "{\"cases\":" << count << ",\"batches\":" << launches << ",\"kernel_ms\":" << runner.kernel_ms
                   << ",\"wall_ms\":" << wall << ",\"batch_capacity\":" << batch << ",\"device\":" << device << "}\n";
            if (!output) throw std::runtime_error("cannot write probe stats");
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
