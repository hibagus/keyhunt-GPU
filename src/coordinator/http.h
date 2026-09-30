#pragma once
#include "keyhunt/coordinator/repository.h"
#include <map>
#include <sys/types.h>
namespace keyhunt::coordination {
struct ServerOptions {
    std::string state_directory,api_socket,admin_socket,authority;
    uid_t proxy_uid=uid_t(-1);
};
int serve(const ServerOptions&);
Json local_admin(const std::string& socket,const Json& request);
}
