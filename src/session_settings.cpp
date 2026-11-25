#include "flock/session_settings.hpp"

namespace flockv0 {
namespace {
void requireText(const Json& value, const std::string& path) {
    if (!value.is_string() || value.get_ref<const std::string&>().find_first_not_of(" \t\r\n") == std::string::npos)
        throw Conflict(path + " must be a nonempty string");
}

void validateWorkspace(const Json& workspace) {
    if (!workspace.is_object()) throw Conflict("session.workspaceStrategy must be an object");
    const auto type = workspace.value("type", Json());
    if (type != "root" && type != "worktree" && type != "existing_worktree")
        throw Conflict("session.workspaceStrategy.type must be root, worktree or existing_worktree");
    if (type == "worktree" && !workspace.contains("baseRef"))
        throw Conflict("session.workspaceStrategy.baseRef is required for a new worktree");
    if (type == "existing_worktree" && !workspace.contains("worktreePath"))
        throw Conflict("session.workspaceStrategy.worktreePath is required for an existing worktree");
    for (const auto& [key, value] : workspace.items()) {
        if (key == "type") continue;
        if (key == "baseRef" || key == "branch" || key == "worktreePath")
            requireText(value, "session.workspaceStrategy." + key);
        else if (key == "startFromOrigin") {
            if (!value.is_boolean()) throw Conflict("session.workspaceStrategy.startFromOrigin must be a boolean");
        } else
            throw Conflict("Unknown session.workspaceStrategy setting: " + key);
    }
}
}

Json normalizeSessionSettings(const Json& session) {
    if (!session.is_object()) throw Conflict("session must be an object");
    if (session.contains("model") != session.contains("providerInstanceId"))
        throw Conflict("Specify both session.model and session.providerInstanceId");

    for (const auto& [key, value] : session.items()) {
        if (key == "model" || key == "providerInstanceId")
            requireText(value, "session." + key);
        else if (key == "runtimeMode") {
            if (value != "approval-required" && value != "auto-accept-edits" && value != "auto" && value != "full-access")
                throw Conflict("Invalid session.runtimeMode");
        } else if (key == "workspaceStrategy")
            validateWorkspace(value);
        else
            throw Conflict("Unknown Session setting: " + key);
    }
    return session;
}
}
