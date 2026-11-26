#include "flock/secret_requests.hpp"
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
#include "database.hpp"

#include <set>
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

std::set<std::string> environments(const Json& routing) {
    std::set<std::string> ids;
    if (auto primary = field(routing, "environmentId"); !primary.empty()) ids.insert(std::move(primary));
    for (const auto& alias : routing.at("config").value("conversationEnvironments", Json::object()))
        if (auto id = conversationEnvironmentId(alias); !id.empty()) ids.insert(std::move(id));
    return ids;
}
}

Json BotSettingsStorage::readiness(std::string_view bot, const Json& routing, const Json& args, T3& t3) {
    Policy policy = permissions;
    const auto& config = routing.at("config");
    const auto name = args.contains("environmentId") ? argument(args, "environmentId") : std::string{};
    std::string id;
    {
        std::lock_guard lock(database.mutex);
        id = taskRouting.taskEnvironment(bot, routing, name);
    }
    auto failure = [&](std::string reason) -> Json { return {{"ready", false}, {"reason", reason}, {"settingsLink", "/settings"}}; };
    if (id.empty()) return failure("Choose a connected T3 environment");
    if (policy.forbidden(bot, Action::environment_launch, id)) return failure("Environment launch denied by installation policy");
    Command command;
    command.ownerBotId = bot;
    command.environmentId = id;
    command.projectId = configuredT3Project(routing, id, name);
    command.config = config;
    command.actionId = actionId();
    command.tool = CommandTool::delegate;
    command.instruction = "Check environment readiness";
    try {
        if (command.projectId.empty()) {
            if (!policy.allows(bot, Action::environment_inspect, id) || !sessionParticipationStorage.t3EnvironmentConnected(id))
                return failure("Check the T3 connection and environment permissions");
            if (t3.projects(id).empty()) return failure("Add or unarchive a project in the T3 environment");
            return {{"ready", true}};
        }
        if (t3.authGeneration(id, config).empty() || t3.preflight(command))
            return failure("Check T3 sign-in, project and connection permissions");
    } catch (...) {
        return failure("Check T3 sign-in and environment settings");
    }
    return {{"ready", true}};
}

Json BotSettingsStorage::botSettingsTool(std::string_view bot, Id sourceMessage, const Json& routing, const Decision& decision) {
    Policy policy = permissions;
    const auto& tool = decision.tool;
    const auto& args = decision.args;
    const auto& config = routing.at("config");
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
                    {"services", config.value("services", Json::array())},
                    {"mcpServers", config.value("mcpServers", Json::array())}};
        auto ids = environments(routing);
        if (field(routing, "environmentId").empty()) ids.merge(taskRouting.registeredT3Environments(bot));
        for (const auto& id : ids) result["environments"].push_back({{"environmentId", id}});
        return result;
    }
    return nullptr;
}
}
