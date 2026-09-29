// Test-only adapter. Hex encoding uses aligned scratch space and verifies the
// extra Int limb so a truncated/noncanonical result cannot accidentally pass.
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include "keyhunt/crypto/secp256k1/IntGroup.h"
#include "keyhunt/core/result_verifier.h"
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
Point point(const std::string& text) {
    Point result;
    result.Clear();
    if (text=="inf") return result;
    if (text.size()!=130 || text.substr(0,2)!="04") throw std::invalid_argument("expected full point");
    result.x=integer(text.substr(2,64));
    result.y=integer(text.substr(66,64));
    result.z.SetInt32(1);
    return result;
}
std::string encode(Point value) {
    if (value.z.IsZero()) return "inf";
    value.Reduce();
    return "04"+encode(value.x)+encode(value.y);
}
void scale(Point& point, const std::string& text) {
    auto z=integer(text);
    if (z.IsZero()) throw std::invalid_argument("zero projective scale");
    point.x.ModMulK1(&z);
    point.y.ModMulK1(&z);
    point.z.ModMulK1(&z);
}
Point sum(Secp256K1& curve, const std::string& op, Point& a, Point& b) {
    if (op=="padd") return curve.Add(a,b);
    if (op=="padd2") return curve.Add2(a,b);
    if (op=="padd_direct") return curve.AddDirect(a,b);
    throw std::invalid_argument("unknown point sum");
}
std::string points(Secp256K1& curve, const std::vector<std::string>& words) {
    const auto& op=words[0];
    if (op=="pub" || op=="rawpub") {
        auto scalar=integer(words.at(1));
        if (op=="rawpub") return encode(curve.ComputePublicKey(&scalar));
        alignas(uint64_t) keyhunt::core::ScalarBytes bytes{};
        scalar.Get32Bytes(bytes.data());
        keyhunt::core::UncompressedPublicKey result{};
        if (!keyhunt::core::CpuResultVerifier(curve).derive(bytes,result)) return "invalid";
        std::string text;
        for (auto byte:result) { text+="0123456789abcdef"[byte>>4]; text+="0123456789abcdef"[byte&15]; }
        return text;
    }
    auto a=point(words.at(1));
    if (op=="pmul") {
        auto scalar=integer(words.at(2));
        const auto result=curve.ScalarMultiplication(a,&scalar);
        a=curve.ScalarMultiplication(a,&scalar);
        return encode(result)+" "+encode(a);
    }
    if (op=="pneg" || op=="pdouble" || op=="pdouble_direct" || op=="preduce") {
        scale(a,words.at(2));
        if (op=="preduce") { a.Reduce(); a.Reduce(); return encode(a); }
        auto apply=[&](Point& p) {
            if (op=="pneg") return curve.Negation(p);
            if (op=="pdouble") return curve.Double(p);
            return curve.DoubleDirect(p);
        };
        const auto result=apply(a);
        a=apply(a);
        return encode(result)+" "+encode(a);
    }
    auto b=point(words.at(2));
    scale(a,words.at(3));
    scale(b,words.at(4));
    const auto result=sum(curve,op,a,b);
    auto left=a,right=b;
    left=sum(curve,op,left,b);
    right=sum(curve,op,a,right);
    return encode(result)+" "+encode(left)+" "+encode(right);
}

void binary(const std::string& op, Int& out, Int& a, Int& b) {
    if (op=="fadd") out.ModAdd(&a,&b);
    else if (op=="fsub") out.ModSub(&a,&b);
    else if (op=="fmul") out.ModMulK1(&a,&b);
    else if (op=="sadd") out.ModAddK1order(&a,&b);
    else throw std::invalid_argument("unknown binary operation");
}
std::string evaluate(Secp256K1& curve, const std::vector<std::string>& words) {
    if (words.size()<2) throw std::invalid_argument("missing arguments");
    const auto& op=words[0];
    if (op[0]=='p' || op=="rawpub") return points(curve,words);
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
