#pragma once

#include "flock/t3_mcp.hpp"

#include <map>

namespace flockv0::t3detail {

struct Failure : std::runtime_error {
    enum class Kind { Auth, Settings, Wire, Tool };
    Kind kind;
    std::string code;
    std::string serverMessage;
    explicit Failure(Kind category, std::string message, std::string toolCode = {}, std::string toolMessage = {})
        : std::runtime_error(std::move(message)), kind(category), code(std::move(toolCode)),
          serverMessage(std::move(toolMessage)) {}
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

inline const Json& member(const Json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end()) invalidResponse();
    return *field;
}
inline const Json& array(const Json& object, const char* key) {
    const auto& field = member(object,key);
    if (!field.is_array()) invalidResponse();
    return field;
}
inline bool boolean(const Json& object, const char* key) {
    const auto& field = member(object,key);
    if (!field.is_boolean()) invalidResponse();
    return field.get<bool>();
}
inline constexpr Id maxHistoryPosition = 9007199254740991LL;
inline Id position(const Json& value) {
    if (!value.is_number_integer() || value < 0 || value > maxHistoryPosition) invalidResponse();
    return value.get<Id>();
}
inline bool localItem(const Json& item, std::string_view threadId) {
    return str(item,"sourceThreadId") == threadId && str(item,"visibility") == "local";
}

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

Credential readCredential(const std::filesystem::path&, const std::string& environment, const Json& config);
std::optional<SessionMetadata> findSession(const std::filesystem::path&, const SessionTarget&, std::string_view query);
std::optional<SessionEventPage> sessionEvents(const std::filesystem::path&, const SessionTarget&, const SessionEventFilter&);
std::optional<SessionEvent> sessionEvent(const std::filesystem::path&, const SessionTarget&, std::string_view itemId);
std::optional<SessionCounts> sessionCounts(const std::filesystem::path&, const SessionTarget&);
std::optional<SessionAttachment> sessionAttachment(const std::filesystem::path&, const SessionTarget&,
                                                   std::string_view itemId, std::string_view attachmentId);

}
