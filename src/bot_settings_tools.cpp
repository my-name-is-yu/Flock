#include "flock/secret_requests.hpp"
#include "t3_connected_environments.hpp"
#include "flock/secret_ref.hpp"
#include "flock/policy.hpp"
#include "task_routing.hpp"
#include "session_participation_storage.hpp"
#include "bot_settings_storage.hpp"
#include "flock/memory_automation.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/decision.hpp"
#include "flock/command.hpp"
#include "flock/workspace_tools.hpp"
#include "database.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
std::string argument(const Json& args, const char* key) {
    auto value = field(args, key);
    if (value.empty() || value.size() > 1024 || value.find_first_of("\n\r") != value.npos || value.find('\0') != value.npos)
        throw Conflict("Invalid tool argument");
    return value;
}

}

Json BotSettingsStorage::readiness(const conversation::Message& message, std::string_view bot, const Json& routing, const Json& args,
                                   T3& t3) {
    Policy policy = permissions;
    const auto& config = routing.at("config");
    const auto name = args.contains("environmentId") ? argument(args, "environmentId") : std::string{};
    const auto resolve = [&](const TaskDestinations* linked) {
        std::lock_guard lock(database.mutex);
        return taskRouting.taskRoute(bot, routing, name, nullptr, linked);
    };
    auto route = resolve(nullptr);
    std::string access;
    if (route.environmentId.empty() && !name.empty()) {
        // A linked environment is reachable only through its gateway, so it is not among the registered connections.
        const auto linked = taskRouting.discoverTaskDestinations(t3, message, false);
        try {
            route = resolve(&linked);
        } catch (const std::runtime_error& error) {
            // The route says why a listed link cannot be used, such as a gateway that is no longer connected.
            return {{"ready", false}, {"reason", error.what()}, {"settingsLink", "/settings"}};
        }
        access = linked.access(route);
    }
    auto failure = [&](std::string reason) -> Json { return {{"ready", false}, {"reason", reason}, {"settingsLink", "/settings"}}; };
    const auto& id = route.environmentId;
    if (id.empty()) return failure("Choose a connected T3 environment");
    if (policy.forbidden(bot, Action::environment_launch, id)) return failure("Environment launch denied by installation policy");
    Command command;
    command.ownerBotId = bot;
    command.environmentId = id;
    command.gatewayEnvironmentId = route.gatewayEnvironmentId;
    command.projectId = std::move(route.projectId);
    command.config = config;
    command.actionId = actionId();
    command.tool = CommandTool::delegate;
    command.instruction = "Check environment readiness";
    try {
        if (command.projectId.empty()) {
            if (!policy.allows(bot, Action::environment_inspect, id) || !t3EnvironmentConnected(database, id))
                return failure("Check the T3 connection and environment permissions");
            if (t3.projects(id).empty()) return failure("Add or unarchive a project in the T3 environment");
            return {{"ready", true}};
        }
        if (t3.authGeneration(command.gatewayEnvironmentId.empty() ? id : command.gatewayEnvironmentId, config).empty() ||
            t3.preflight(command))
            return failure("Check T3 sign-in, project and connection permissions");
    } catch (...) {
        return failure("Check T3 sign-in and environment settings");
    }
    Json ready{{"ready", true}};
    if (!access.empty()) ready["access"] = access;
    return ready;
}

Json BotSettingsStorage::botSettingsTool(std::string_view bot, Id sourceMessage, const BotTools& settings, const Decision& decision) {
    Policy policy = permissions;
    const auto& tool = decision.tool;
    const auto& args = decision.args;
    if (tool == "list_secrets") {
        auto rows = database.query("SELECT name FROM protected_inputs WHERE status='fulfilled' ORDER BY name");
        auto names = Json::array();
        while (rows.next())
            if (!policy.forbidden(bot, Action::secret_use, rows.string(0))) names.push_back(rows.string(0));
        return {{"names", names}};
    }
    if (tool == "request_secret_input") {
        const SecretRef ref{argument(args, "name")};
        if (!ref.valid()) throw Conflict("Invalid secret reference");
        if (database.query("SELECT 1 FROM protected_inputs WHERE name=? AND status='fulfilled'", {ref.name}).next()) {
            if (!policy.allows(bot, Action::secret_use, ref.name)) throw AccessDenied("Protected input unavailable");
            return {{"status", "available"}};
        }
        const auto id = secretRequests.requestSecret(bot, ref, sourceMessage);
        return {{"requestId", id}, {"status", "pending"}, {"settingsLink", "/settings/" + std::string(bot) + "#secrets"}};
    }
    if (tool == "list_connections") {
        Json result{{"environments", Json::array()},
                    {"mcpServers", Json::array()},
                    {"skills", Json::array()},
                    {"webSearchConfigured", settings.webSearchConfigured}};
        for (const auto& id : taskRouting.registeredT3Environments(bot)) result["environments"].push_back({{"environmentId", id}});
        if (settings.mcp)
            for (const auto& name : settings.mcp->serverNames())
                if (!policy.forbidden(bot, Action::mcp_discover, name)) result["mcpServers"].push_back(name);
        for (const auto& [name, path] : settings.skills)
            if (policy.allows(bot, Action::skill_read, name)) result["skills"].push_back(name);
        return result;
    }
    return nullptr;
}
}
