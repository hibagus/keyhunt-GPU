#include "keyhunt/coordinator/worker.h"
#include "protocol.h"
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace keyhunt::coordination {
namespace {
size_t receive(char* data,size_t size,size_t count,void* context){
    auto& output=*static_cast<std::string*>(context);
    if(size&&count>SIZE_MAX/size)return 0;
    const auto bytes=size*count;
    if(bytes>8*1024*1024-output.size())return 0;
    try{output.append(data,bytes);return bytes;}catch(...){return 0;}
}
struct CurlGlobal {
    CurlGlobal(){if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("cannot initialize HTTPS");}
    ~CurlGlobal(){curl_global_cleanup();}
};
}
Json https_request(const Json& config,const std::string& method,const std::string& path,const Json& body){
    static CurlGlobal global;
    using namespace wire;
    const auto endpoint=str(config,"endpoint",512);
    if(endpoint.rfind("https://",0)!=0||endpoint.find_first_of("/?#@",8)!=std::string::npos||
       path.rfind("/api/v1/",0)!=0||path.find_first_of("\r\n#")!=std::string::npos||(method!="GET"&&method!="POST"))
        throw std::invalid_argument("HTTPS requests require an API path and HTTPS authority");
    const auto key=str(config,"key",4096);struct stat st{};
    if(lstat(key.c_str(),&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))
        throw std::runtime_error("client private key must be an owned private regular file");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if(!curl)throw std::runtime_error("cannot initialize HTTPS request");
    curl_slist* raw=nullptr;
    raw=curl_slist_append(raw,"Content-Type: application/json");
    raw=curl_slist_append(raw,"Expect:");
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(raw,curl_slist_free_all);
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> resolve(nullptr,curl_slist_free_all);
    if(config.contains("resolve"))resolve.reset(curl_slist_append(nullptr,str(config,"resolve",1024).c_str()));
    const auto url=endpoint+path,ca=str(config,"ca",4096),cert=str(config,"certificate",4096),payload=body.dump();
    std::string response;char diagnostic[CURL_ERROR_SIZE]{};
    // No insecure mode, redirects, environmental proxy or HTTP fallback. A DNS
    // override changes only routing: peer and hostname verification stay enabled.
    curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS,long(CURLPROTO_HTTPS));
    curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl.get(),CURLOPT_PROXY,"");
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
    curl_easy_setopt(curl.get(),CURLOPT_SSLVERSION,long(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(curl.get(),CURLOPT_CAINFO,ca.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_SSLCERT,cert.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_SSLKEY,key.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_RESOLVE,resolve.get());
    curl_easy_setopt(curl.get(),CURLOPT_HTTPHEADER,headers.get());
    curl_easy_setopt(curl.get(),CURLOPT_HTTP_VERSION,long(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(curl.get(),CURLOPT_ERRORBUFFER,diagnostic);
    if(method=="POST"){
        if(payload.size()>8*1024*1024)throw std::invalid_argument("HTTPS request exceeds page limit");
        curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDS,payload.data());
        curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDSIZE_LARGE,curl_off_t(payload.size()));
    }
    const auto result=curl_easy_perform(curl.get());
    if(result!=CURLE_OK)throw std::runtime_error(std::string("HTTPS request failed: ")+diagnostic);
    long status=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    if(status!=200){
        std::string reason="HTTPS API request rejected";
        try{const auto parsed=parse_json(response);if(parsed.contains("error"))reason=str(parsed,"error",512);}catch(const std::exception&){}
        throw Error(int(status),reason);
    }
    return parse_json(response);
}
}
