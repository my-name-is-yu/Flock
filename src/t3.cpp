#include "flock/t3.hpp"
#include "flock/session_prompt.hpp"
#include "flock/t3_session.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace flockv0 {
std::string sessionPrompt(const std::string& actionId, const std::string& instruction) {
    const auto marker = "\n\n[Flock action ID: " + actionId + "]";
    return instruction.ends_with(marker) ? instruction : instruction + marker;
}

InterruptOutcome T3::interruptSessionMessage(const std::string& environmentId, const RunRef& run) {
    return interrupt(environmentId, Json::object(), run);
}

Excerpt T3::read(const ReadRequest&) {
    throw std::runtime_error("Session reading is unavailable");
}
std::optional<SessionMetadata> T3::sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) {
    return {};
}
std::optional<SessionEventPage> T3::sessionEvents(const SessionTarget&, const SessionEventFilter&) {
    return {};
}
std::optional<SessionEvent> T3::sessionEvent(const SessionTarget&, std::string_view) {
    return {};
}
std::optional<SessionCounts> T3::sessionCounts(const SessionTarget&) {
    return {};
}
std::optional<SessionAttachment> T3::sessionAttachment(const SessionTarget&, std::string_view, std::string_view) {
    return {};
}
}
