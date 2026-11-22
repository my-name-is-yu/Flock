#pragma once

#include "flock/t3_mcp.hpp"

#include <map>

namespace flockv0::t3detail {

inline constexpr auto ProtocolVersion = "2025-06-18";
inline Json initializeParams() {
    return {{"protocolVersion", ProtocolVersion}, {"capabilities", Json::object()},
            {"clientInfo", {{"name", "Flock"}, {"version", "0.1.0"}}}};
}

struct Failure : std::runtime_error {
    enum class Kind { Auth, Settings, Wire, Rpc, Tool };
    enum class Reason { Other, QuestionGone, WorkSettings };
    Kind kind;
    Reason reason;
    explicit Failure(Kind category, std::string message, Reason reason = Reason::Other)
        : std::runtime_error(std::move(message)), kind(category), reason(reason) {}
};

using Headers = std::map<std::string, std::string>;
struct HttpResponse {
    long status = 0;
    Headers headers;
    std::string body;
};
struct Url { std::string full, origin, path; };
Url checkedUrl(const std::string& value);
HttpResponse http(const std::string& url, std::string_view method, const Headers& headers = {},
                  const std::string& body = {}, long timeoutMs = 6000);
std::string encode(std::string_view value);
std::string decode(std::string_view value);
std::string trim(std::string_view value);
std::string str(const Json& object, std::string_view name);
Json parse(std::string_view body);
[[noreturn]] void invalidResponse();

struct Credential {
    std::string mcpUrl, token, generation;
    Seconds expiresAt = 0;
    bool operate = false;
};
Credential loadCredential(const std::filesystem::path&, const std::string& environmentId);

class Client {
public:
    explicit Client(Credential credential);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Json call(std::string_view tool, Json arguments);
private:
    Json rpc(std::string_view method, const Json& arguments);
    HttpResponse request(std::string_view method, const std::string& body, long timeoutMs = 6000);
    Credential credential_;
    Headers headers_;
    unsigned nextId_ = 0;
};

}
