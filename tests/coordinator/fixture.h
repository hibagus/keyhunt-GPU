#pragma once
#include "../storage/checkpoint_fixture.h"
#include "keyhunt/coordinator/repository.h"
#include "protocol.h"
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
namespace cfixture {
using namespace fixture;
using namespace keyhunt::coordination;
inline std::string pem(int serial,int64_t before,int64_t after,bool client_auth=true){
    // Ephemeral test keys stay in memory; certificates need no external CA for
    // repository tests. The Apache gate separately verifies real chains.
    std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC,nullptr),EVP_PKEY_CTX_free);
    require(ctx&&EVP_PKEY_keygen_init(ctx.get())==1&&EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(),NID_X9_62_prime256v1)==1,"test key init");
    EVP_PKEY* raw=nullptr;require(EVP_PKEY_keygen(ctx.get(),&raw)==1,"test key generation");
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(raw,EVP_PKEY_free);
    std::unique_ptr<X509,decltype(&X509_free)> cert(X509_new(),X509_free);
    X509_set_version(cert.get(),2);ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),serial);
    ASN1_TIME_set(X509_getm_notBefore(cert.get()),time_t(before));ASN1_TIME_set(X509_getm_notAfter(cert.get()),time_t(after));
    X509_set_pubkey(cert.get(),key.get());auto* name=X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("same untrusted name"),-1,-1,0);
    X509_set_issuer_name(cert.get(),name);
    for(const auto& spec:std::vector<std::pair<int,const char*>>{{NID_basic_constraints,"critical,CA:FALSE"},
        {NID_key_usage,"critical,digitalSignature"},{NID_ext_key_usage,client_auth?"clientAuth":"serverAuth"}}){
        auto* ext=X509V3_EXT_conf_nid(nullptr,nullptr,spec.first,const_cast<char*>(spec.second));
        require(ext&&X509_add_ext(cert.get(),ext,-1)==1,"test certificate extension");X509_EXTENSION_free(ext);
    }
    require(X509_sign(cert.get(),key.get(),EVP_sha256())>0,"test certificate signing");
    std::unique_ptr<BIO,decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()),BIO_free);PEM_write_bio_X509(bio.get(),cert.get());
    char* data=nullptr;const auto size=BIO_get_mem_data(bio.get(),&data);return std::string(data,size_t(size));
}
template<class F>void denied(int expected,F fn){
    try{fn();}catch(const Error& e){require(e.status==expected,e.what());return;}
    throw std::runtime_error("expected coordinator denial");
}
inline Json job_input(const core::XPointTargets& targets){
    const auto b=keyhunt::storage::detail::binding(targets);
    return {{"mode","xpoint"},{"begin",UInt256(1).hex()},{"end_exclusive",UInt256(101).hex()},
        {"block_width",UInt256(10).hex()},{"configuration",wire::hex(b.configuration)},{"targets",wire::hex(b.targets)}};
}
}
