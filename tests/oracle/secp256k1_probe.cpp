// Public synthetic vectors only. No keyhunt arithmetic is linked into this tool.
#include <secp256k1.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<unsigned char> decode(const std::string& text) {
    if (text.size() % 2) throw std::invalid_argument("odd hex length");
    std::vector<unsigned char> result;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c-'0';
        if (c >= 'a' && c <= 'f') return c-'a'+10;
        throw std::invalid_argument("bad hex digit");
    };
    for (size_t i=0; i<text.size(); i+=2) result.push_back(digit(text[i])*16+digit(text[i+1]));
    return result;
}
std::string serialize(secp256k1_context* context, const secp256k1_pubkey& pub) {
    unsigned char bytes[65];
    size_t size = sizeof(bytes);
    if (!secp256k1_ec_pubkey_serialize(context, bytes, &size, &pub, SECP256K1_EC_UNCOMPRESSED))
        throw std::runtime_error("serialize failed");
    std::string result;
    for (size_t i=0; i<size; ++i) {
        result += "0123456789abcdef"[bytes[i]>>4];
        result += "0123456789abcdef"[bytes[i]&15];
    }
    return result;
}
std::string evaluate(secp256k1_context* context, const std::vector<std::string>& words) {
    if (words.size()<2) throw std::invalid_argument("missing input");
    const auto& op=words[0];
    secp256k1_pubkey a, b;
    if (op=="pub" && words.size()==2) {
        const auto scalar=decode(words[1]);
        if (scalar.size()!=32 || !secp256k1_ec_pubkey_create(context, &a, scalar.data())) return "invalid";
        return serialize(context, a);
    }
    auto parse=[&](const std::string& word, secp256k1_pubkey& point) {
        const auto bytes=decode(word);
        return !bytes.empty() && secp256k1_ec_pubkey_parse(context, &point, bytes.data(), bytes.size());
    };
    const bool infinity_a=words[1]=="inf";
    if (!infinity_a && !parse(words[1],a)) return "invalid";
    if (op=="parse" && words.size()==2) return infinity_a ? "invalid" : serialize(context,a);
    if (op=="neg" && words.size()==2) {
        if (infinity_a) return "inf";
        if (!secp256k1_ec_pubkey_negate(context,&a)) throw std::runtime_error("negation failed");
        return serialize(context,a);
    }
    if (op=="add" && words.size()==3) {
        const bool infinity_b=words[2]=="inf";
        if (!infinity_b && !parse(words[2],b)) return "invalid";
        if (infinity_a) return infinity_b ? "inf" : serialize(context,b);
        if (infinity_b) return serialize(context,a);
        const secp256k1_pubkey* points[]={&a,&b};
        secp256k1_pubkey sum;
        if (!secp256k1_ec_pubkey_combine(context,&sum,points,2)) return "inf";
        return serialize(context,sum);
    }
    throw std::invalid_argument("unsupported operation");
}
}
int main() {
    auto* context=secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    for (std::string line; std::getline(std::cin,line);) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word; input>>word;) words.push_back(word);
        try { std::cout<<evaluate(context,words)<<'\n'; }
        catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; secp256k1_context_destroy(context); return 1; }
    }
    secp256k1_context_destroy(context);
}
