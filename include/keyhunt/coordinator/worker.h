#pragma once
#include "keyhunt/coordinator/repository.h"
namespace keyhunt::coordination {
// One machine owns one immutable pending request. Transport is injected so the
// same scheduler/outbox path can be tested without GPUs or a network clock.
class Worker {
public:
    using Transport=std::function<Json(const Json&)>;
    using Timer=std::function<int64_t()>;
    explicit Worker(const std::string& directory,storage::Journal::Clock={},Timer={});
    ~Worker();
    void configure(const Json&);
    Json configuration() const;
    bool synchronize(const Transport&,bool manual=false);
    std::optional<storage::Grant> next(const std::string& device) const;
    // Held for this Worker's lifetime. Rebinding a UUID is explicit and only
    // succeeds after the previous process releases its device lock.
    void acquire_device(const std::string& device,const std::string& uuid,bool rebind=false);
    std::optional<storage::Grant> claim_device();
    Json execution(const storage::Grant&) const;
    Json status() const;
    Json execution(const std::string& device) const;
    storage::Journal& journal();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Verified HTTPS only; resolve optionally overrides DNS, preserving Host/SNI.
Json https_request(const Json& configuration,const std::string& method,
                   const std::string& path,const Json& body=Json::object());
}
