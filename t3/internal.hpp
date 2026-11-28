#pragma once

#include "flock/json.hpp"
#include "flock/text.hpp"
#include "flock/session_ref.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0::recording {
class Capture;
}

namespace flockv0 {
struct SessionAttachment;
struct SessionCounts;
struct SessionEvent;
struct SessionEventFilter;
struct SessionEventPage;
struct SessionMetadata;
}

namespace flockv0::t3detail {

inline constexpr auto ProtocolVersion = "2025-06-18";
inline Json initializeParams() {
    return {
        {"protocolVersion", ProtocolVersion}, {"capabilities", Json::object()}, {"clientInfo", {{"name", "Flock"}, {"version", "0.1.0"}}}};
}

struct Failure : std::runtime_error {
    enum class Kind { Auth, Settings, Wire, Rpc, Tool };
    enum class Reason {
        Other,
        QuestionGone,
        WorkSettings,
        ThreadNotFound,
        HistoryNotFound,
        ProjectNotFound,
        Timeout,
        DelegatedFromRejected,
        CredentialExpired,
        LinkedEnvironmentUnavailable
    };
    Kind kind;
    Reason reason;
    explicit Failure(Kind category, std::string message, Reason reason = Reason::Other)
        : std::runtime_error(std::move(message)), kind(category), reason(reason) {}
    bool missingHistory() const { return reason == Reason::ThreadNotFound || reason == Reason::HistoryNotFound; }
};

using Headers = std::map<std::string, std::string>;
struct HttpResponse {
    long status = 0;
    Headers headers;
    std::string body;
};
struct Url {
    std::string full, origin, path;
};
Url checkedUrl(const std::string& value);
HttpResponse http(const std::string& url, std::string_view method, const Headers& headers = {}, const std::string& body = {},
                  long timeoutMs = 6000);
std::string decode(std::string_view value);
Json parse(std::string_view body);
[[noreturn]] void invalidResponse();

inline const Json& member(const Json& object, const char* key) {
    const auto field = object.find(key);
    if (field == object.end()) invalidResponse();
    return *field;
}
inline const Json& array(const Json& object, const char* key) {
    const auto& field = member(object, key);
    if (!field.is_array()) invalidResponse();
    return field;
}
inline bool boolean(const Json& object, const char* key) {
    const auto& field = member(object, key);
    if (!field.is_boolean()) invalidResponse();
    return field.get<bool>();
}
inline constexpr Id maxHistoryPosition = 9007199254740991LL;
inline Id position(const Json& value) {
    if (!value.is_number_integer() || value < 0 || value > maxHistoryPosition) invalidResponse();
    return value.get<Id>();
}
inline bool localItem(const Json& item, std::string_view threadId) {
    return field(item, "sourceThreadId") == threadId && field(item, "visibility") == "local";
}

struct Credential {
    std::string mcpUrl, token, generation;
    Seconds expiresAt = 0;
    bool operate = false;
    std::string label;
};
struct WorkspaceHead {
    std::string commit;
    bool clean = true;
};
Credential loadCredential(const std::filesystem::path&, const std::string& environmentId);

class Client {
public:
    explicit Client(Credential credential, SessionReadDeadline deadline = {}, std::string targetEnvironmentId = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Json call(std::string_view tool, Json arguments);
private:
    Json rpc(std::string_view method, const Json& arguments);
    void record(recording::Capture&, unsigned id, std::string_view method, const Json& arguments, const Json& message,
                std::string_view contentType, long status) noexcept;
    HttpResponse request(std::string_view method, const std::string& body, long timeoutMs = 6000);
    Credential credential_;
    SessionReadDeadline deadline_;
    std::string targetEnvironmentId_;
    Headers headers_;
    unsigned nextId_ = 0;
    std::string recordingVersion_;
};

Json readThread(Client&, std::string_view threadId, std::string_view view, unsigned limit, int chars, Json args = Json::object(),
                int runLimit = 1);
void advancePosition(const Json&, Id&);
bool gitCommit(std::string_view sha);
bool loopbackGitHost(std::string_view origin);
std::optional<WorkspaceHead> workspaceHead(Client&, std::string_view projectId, const Json& thread);
bool moreHistory(const Json&, Id after, Id start);
std::optional<SessionMetadata> sessionMetadata(const std::filesystem::path&, const SessionTarget&, bool includeMessages,
                                               SessionReadDeadline);
std::optional<SessionEventPage> sessionEvents(const std::filesystem::path&, const SessionTarget&, const SessionEventFilter&);
std::optional<SessionEvent> sessionEvent(const std::filesystem::path&, const SessionTarget&, std::string_view itemId);
std::optional<SessionCounts> sessionCounts(const std::filesystem::path&, const SessionTarget&);
std::optional<SessionAttachment> sessionAttachment(const std::filesystem::path&, const SessionTarget&, std::string_view itemId,
                                                   std::string_view attachmentId);

}
