#include "http.hpp"

#include <nlohmann/json.hpp>

namespace flockv0::web {
void reply(httplib::Response& res, int status, const Json& body) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, Json::error_handler_t::replace), "application/json; charset=utf-8");
}

void error(httplib::Response& res, int status, std::string_view code) {
    reply(res, status, {{"error", code}});
}

void error(httplib::Response& res, int status, std::string_view code, std::string_view message) {
    reply(res, status, {{"error", code}, {"message", message}});
}
}
