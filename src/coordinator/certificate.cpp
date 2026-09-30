#include "keyhunt/coordinator/repository.h"
#include "sqlite.h"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>

namespace keyhunt::coordination {
namespace {
template<class T,void(*Free)(T*)>using Owned=std::unique_ptr<T,decltype(Free)>;
int64_t timestamp(const ASN1_TIME* value){
    std::tm t{};if(ASN1_TIME_to_tm(value,&t)!=1)throw Error(401,"invalid certificate validity");
    auto n=timegm(&t);if(n<0)throw Error(401,"unsupported certificate date");return int64_t(n);
}
std::string bio_text(BIO* bio){char* p=nullptr;const auto n=BIO_get_mem_data(bio,&p);return std::string(p,size_t(n));}
}
Certificate certificate(const std::string& pem){
    if(pem.empty()||pem.size()>16384)throw Error(401,"invalid certificate size");
    std::unique_ptr<BIO,decltype(&BIO_free)> input(BIO_new_mem_buf(pem.data(),int(pem.size())),BIO_free);
    Owned<X509,X509_free> cert(PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr),X509_free);
    if(!cert)throw Error(401,"malformed client certificate");
    const auto trailing=bio_text(input.get());
    for(unsigned char c:trailing)if(!std::isspace(c))throw Error(401,"multiple or trailing certificate data");
    if(X509_check_ca(cert.get()) || X509_check_purpose(cert.get(),X509_PURPOSE_SSL_CLIENT,0)!=1)
        throw Error(401,"certificate is not a client leaf");
    auto* raw_usage=static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(cert.get(),NID_ext_key_usage,nullptr,nullptr));
    Owned<EXTENDED_KEY_USAGE,EXTENDED_KEY_USAGE_free> usage(raw_usage,EXTENDED_KEY_USAGE_free);
    bool client_auth=false;
    if(usage)for(int i=0;i<sk_ASN1_OBJECT_num(usage.get());++i)
        client_auth|=OBJ_obj2nid(sk_ASN1_OBJECT_value(usage.get(),i))==NID_client_auth;
    if(!client_auth)throw Error(401,"explicit clientAuth usage required");
    Certificate out;out.not_before=timestamp(X509_get0_notBefore(cert.get()));out.not_after=timestamp(X509_get0_notAfter(cert.get()));
    if(out.not_after<=out.not_before)throw Error(401,"invalid certificate lifetime");
    const int n=i2d_X509(cert.get(),nullptr);if(n<=0)throw Error(401,"cannot encode certificate");
    out.der.resize(size_t(n));auto* p=out.der.data();i2d_X509(cert.get(),&p);
    out.fingerprint=storage::detail::digest(out.der);
    const auto* public_key=X509_get_X509_PUBKEY(cert.get());const int k=i2d_X509_PUBKEY(public_key,nullptr);
    if(k<=0)throw Error(401,"cannot encode certificate public key");
    std::vector<uint8_t> public_der(size_t(k),0);p=public_der.data();i2d_X509_PUBKEY(public_key,&p);
    out.spki=storage::detail::digest(public_der);
    std::unique_ptr<BIO,decltype(&BIO_free)> issuer(BIO_new(BIO_s_mem()),BIO_free);
    X509_NAME_print_ex(issuer.get(),X509_get_issuer_name(cert.get()),0,XN_FLAG_RFC2253);out.issuer=bio_text(issuer.get());
    Owned<BIGNUM,BN_free> serial(ASN1_INTEGER_to_BN(X509_get_serialNumber(cert.get()),nullptr),BN_free);
    if(!serial)throw Error(401,"invalid certificate serial");
    char* raw=BN_bn2hex(serial.get());if(!raw)throw Error(401,"cannot encode certificate serial");
    out.serial=raw;OPENSSL_free(raw);std::transform(out.serial.begin(),out.serial.end(),out.serial.begin(),[](unsigned char c){return char(std::tolower(c));});
    return out;
}
std::string base64(const std::string& input){
    std::string out(4*((input.size()+2)/3),'\0');
    if(!input.empty())EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<const unsigned char*>(input.data()),int(input.size()));
    return out;
}
std::string unbase64(const std::string& input){
    if(input.empty()||input.size()>32768||input.size()%4)throw Error(401,"invalid certificate encoding");
    const size_t padding=(input.back()=='=')+(input.size()>1&&input[input.size()-2]=='=');
    for(size_t i=0;i<input.size()-padding;++i){const auto c=input[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'))throw Error(401,"invalid certificate encoding");}
    std::string out(3*(input.size()/4),'\0');
    const int n=EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<const unsigned char*>(input.data()),int(input.size()));
    if(n<0||size_t(n)<padding)throw Error(401,"invalid certificate encoding");
    out.resize(size_t(n)-padding);
    if(base64(out)!=input)throw Error(401,"noncanonical certificate encoding");
    return out;
}
Json parse_json(const std::string& input){
    if(input.size()>8*1024*1024)throw Error(413,"JSON body too large");
    std::vector<std::set<std::string>> keys;
    try{return Json::parse(input,[&](int depth,Json::parse_event_t event,Json& value){
        if(depth>32)throw Error(400,"JSON nesting too deep");
        if(event==Json::parse_event_t::object_start)keys.emplace_back();
        if(event==Json::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)throw Error(400,"duplicate JSON key");
        if(event==Json::parse_event_t::object_end)keys.pop_back();
        return true;
    });}catch(const Json::exception&){throw Error(400,"malformed JSON");}
}
} // namespace keyhunt::coordination
