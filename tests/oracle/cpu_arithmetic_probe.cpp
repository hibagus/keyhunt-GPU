// Test-only adapter. Hex encoding uses aligned scratch space and verifies the
// extra Int limb so a truncated/noncanonical result cannot accidentally pass.
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include "keyhunt/crypto/secp256k1/IntGroup.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
Int integer(const std::string& text) {
    if (text.size()!=64 || text.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::invalid_argument("expected fixed-width scalar/field hex");
    Int result;
    result.SetBase16(text.c_str());
    return result;
}
std::string encode(Int value) {
    if (value.bits64[4]) return "noncanonical-wide";
    alignas(uint64_t) unsigned char bytes[32];
    value.Get32Bytes(bytes);
    std::string result;
    for (auto byte:bytes) {
        result += "0123456789abcdef"[byte>>4];
        result += "0123456789abcdef"[byte&15];
    }
    return result;
}
void binary(const std::string& op, Int& out, Int& a, Int& b) {
    if (op=="fadd") out.ModAdd(&a,&b);
    else if (op=="fsub") out.ModSub(&a,&b);
    else if (op=="fmul") out.ModMulK1(&a,&b);
    else if (op=="sadd") out.ModAddK1order(&a,&b);
    else throw std::invalid_argument("unknown binary operation");
}
std::string evaluate(Secp256K1&, const std::vector<std::string>& words) {
    if (words.size()<2) throw std::invalid_argument("missing arguments");
    const auto& op=words[0];
    auto a=integer(words[1]);
    if (words.size()==2) {
        auto result=a;
        if (op=="fmulself") result.ModMulK1(&result,&result);
        else if (op=="smulself") result.ModMulK1order(&result);
        else if (op=="finv") result.ModInv();
        else if (op=="fneg") result.ModNeg();
        else if (op=="fsquare") {
            result.ModSquareK1(&a);
            auto alias=a;
            alias.ModSquareK1(&alias);
            return encode(result)+" "+encode(alias);
        } else throw std::invalid_argument("unknown unary operation");
        return encode(result);
    }
    if (words.size()!=3) throw std::invalid_argument("wrong arity");
    auto b=integer(words[2]);
    if (op=="smul") {
        auto left=a, right=b;
        left.ModMulK1order(&b);
        right.ModMulK1order(&a);
        return encode(left)+" "+encode(right);
    }
    Int result;
    binary(op,result,a,b);
    auto left=a, right=b;
    binary(op,left,left,b);
    binary(op,right,a,right);
    std::string response=encode(result)+" "+encode(left)+" "+encode(right);
    if (op=="fmul") {
        auto inplace=a;
        inplace.ModMulK1(&b);
        response+=" "+encode(inplace);
    }
    return response;
}
}
int main() {
    auto curve=std::make_unique<Secp256K1>();
    curve->Init();
    for (std::string line; std::getline(std::cin,line);) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word; input>>word;) words.push_back(word);
        try { const auto result=evaluate(*curve,words); std::cout<<result<<'\n'; }
        catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    }
}
