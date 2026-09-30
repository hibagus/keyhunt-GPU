#pragma once
#include "keyhunt/storage/journal.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace keyhunt::coordination {
using Json=nlohmann::json;
struct Error:std::runtime_error {
    int status;
    Error(int code,const std::string& message):std::runtime_error(message),status(code){}
};
struct Certificate {
    std::vector<uint8_t> der,fingerprint,spki;
    std::string issuer,serial;
    int64_t not_before=0,not_after=0;
};
Certificate certificate(const std::string& pem);
std::string base64(const std::string&);
std::string unbase64(const std::string&);
Json parse_json(const std::string&);

// Trusted local API. The network service alone supplies the verified certificate
// and gates admin() using a separate same-UID Unix socket. Workers cannot select
// an actor or call administrative operations through request().
class Repository {
public:
    explicit Repository(const std::string& directory,storage::Journal::Clock={});
    ~Repository();
    Json admin(const Json&);
    Json control_snapshot(const Certificate&,const Json& sync_request);
    Json request(const Certificate&,const std::string& method,const std::string& path,const Json& body=Json::object());
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    void test_page_limit(bool constrained);
#endif
    int64_t now() const;
    std::string directory() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace keyhunt::coordination
