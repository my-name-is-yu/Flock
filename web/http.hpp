#pragma once
#include "flock/json.hpp"
#include "flock/person.hpp"
#include <httplib.h>

#include <cstddef>
#include <string_view>

namespace flockv0 {
struct WebCallbacks;
}

namespace flockv0::web {
inline constexpr std::size_t maxStandardPayload = 128 * 1024;
inline constexpr std::size_t maxUploadPayload = 7 * 1024 * 1024 + maxStandardPayload;

void reply(httplib::Response&, int status, const Json& body);
void error(httplib::Response&, int status, std::string_view code);
void error(httplib::Response&, int status, std::string_view code, std::string_view message);

Person person(const WebCallbacks&, const httplib::Request&);
bool hasWebIdentities(const WebCallbacks&);
std::string_view conversationMediaKind(std::string_view mime);
}
