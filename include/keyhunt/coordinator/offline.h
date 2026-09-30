#pragma once
#include "keyhunt/coordinator/repository.h"

namespace keyhunt::coordination {
// Portable files use deterministic JSON plus one newline. The checksum protects
// the bytes in transit; the operator must obtain it through a trusted channel.
std::string offline_checksum(const Json& document);
std::string publish_offline(const std::string& path,const Json& document);
Json read_offline(const std::string& path,const std::string& expected_sha256);
// The coordinator and courier share the same strict portable envelope contract.
void validate_offline_request(const Json&);
Json offline_response(const Json& request,int status,const Json& body);
}
