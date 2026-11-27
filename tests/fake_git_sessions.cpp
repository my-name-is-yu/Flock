#include "fake_git_sessions.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"

namespace git_fixture {
std::optional<SessionMetadata> Sessions::sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) {
    if (!askOrigin || target != *askOrigin) return std::nullopt;
    return SessionMetadata{.title = "Original request", .project = "Fixture project", .projectId = "project"};
}

Delivery Sessions::send(const Command& command) {
    if (command.tool == CommandTool::message_session) {
        sessionSends.push_back(command);
        if (afterSessionSend) afterSessionSend();
        return {Delivery::Kind::Accepted,
                {command.target.threadId, "followup-" + std::to_string(sessionSends.size()), {}, command.gatewayEnvironmentId},
                {},
                false,
                "queued"};
    }
    const std::string prefix = holdRuns ? "held-" : "";
    return {Delivery::Kind::Accepted,
            {prefix + "thread-" + command.projectId, prefix + "run-" + command.projectId, {}, command.gatewayEnvironmentId},
            {}};
}
}
