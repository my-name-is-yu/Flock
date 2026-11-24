#include "storage.hpp"
#include <set>

namespace flockv0 {
namespace {
std::string argument(const Json& args, const char* key) {
    auto value = field(args,key);
    if (value.empty() || value.size()>1024 || value.find_first_of("\n\r") != value.npos ||
        value.find('\0') != value.npos) throw Conflict("Invalid tool argument");
    return value;
}

std::set<std::string> environments(const Json& routing) {
    std::set<std::string> ids;
    if (auto primary = field(routing,"environmentId"); !primary.empty()) ids.insert(std::move(primary));
    for (const auto& alias : routing.at("config").value("conversationEnvironments",Json::object()))
        if (auto id = conversationEnvironmentId(alias); !id.empty()) ids.insert(std::move(id));
    return ids;
}
}

Seconds Store::Impl::environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const {
    auto row = query("SELECT timeout FROM environment_settings WHERE bot=? AND environment=?",{bot,environment});
    return row.next() ? row.number(0) : config.value("runTimeoutSeconds",Seconds{1800});
}

Json Store::Impl::readiness(std::string_view bot, const Json& routing, const Json& args, T3& t3) {
    Policy policy(*this);
    const auto& config = routing.at("config");
    const auto name = args.contains("environmentId") ? argument(args,"environmentId") : std::string{};
    std::string id;
    {
        std::lock_guard lock(mutex);
        id = taskEnvironment(bot,routing,name);
    }
    auto failure = [&](std::string reason) -> Json {return {{"ready",false},{"reason",reason},{"settingsLink","/settings"}};};
    if (id.empty()) return failure("Choose a connected T3 environment");
    if (policy.forbidden(bot,Action::environment_launch,id)) return failure("Environment launch denied by installation policy");
    Command command;
    command.ownerBotId = bot;
    command.environmentId = id;
    command.projectId = configuredT3Project(routing,id,name);
    command.config = config;
    command.actionId = actionId();
    command.tool = "delegate";
    command.instruction = "Check environment readiness";
    try {
        if (command.projectId.empty()) {
            if (!policy.allows(bot,Action::environment_inspect,id) || !t3EnvironmentConnected(id))
                return failure("Check the T3 connection and environment permissions");
            if (t3.projects(id).empty()) return failure("Add or unarchive a project in the T3 environment");
            return {{"ready",true}};
        }
        if (t3.authGeneration(id,config).empty() || t3.preflight(command)) return failure("Check T3 sign-in, project and connection permissions");
    } catch (...) { return failure("Check T3 sign-in and environment settings"); }
    return {{"ready",true}};
}

Json Store::Impl::botSettingsTool(std::string_view bot, const Person& actor, Id sourceMessage, const Json& routing, const Decision& decision) {
    Policy policy(*this);
    const auto& tool = decision.tool;
    const auto& args = decision.args;
    const auto& config = routing.at("config");
    if (tool == "list_secrets") {
        auto rows = query("SELECT name FROM protected_inputs WHERE status='fulfilled' ORDER BY name");
        auto names = Json::array();
        while (rows.next())
            if (!policy.forbidden(bot,Action::secret_use,rows.string(0))) names.push_back(rows.string(0));
        return {{"names",names}};
    }
    if (tool == "request_secret_input") {
        const SecretRef ref{argument(args,"name")};
        if (!ref.valid()) throw Conflict("Invalid secret reference");
        if (query("SELECT 1 FROM protected_inputs WHERE name=? AND status='fulfilled'",{ref.name}).next()) {
            if (!policy.allows(bot,Action::secret_use,ref.name)) throw AccessDenied("Protected input unavailable");
            return {{"status","available"}};
        }
        const auto id = policy.requestSecret(bot,ref,sourceMessage);
        return {{"requestId",id},{"status","pending"},{"settingsLink","/settings/"+std::string(bot)+"#secrets"}};
    }
    if (tool == "list_connections") {
        Json result{{"environments",Json::array()},{"services",config.value("services",Json::array())},
                    {"mcpServers",config.value("mcpServers",Json::array())}};
        auto ids = environments(routing);
        if (field(routing,"environmentId").empty()) ids.merge(registeredT3Environments(bot));
        for (const auto& id : ids) {
            Json entry{{"environmentId",id}};
            if (policy.allows(bot,Action::environment_inspect,id))
                entry["runTimeoutSeconds"] = environmentTimeout(bot,id,config);
            result["environments"].push_back(entry);
        }
        return result;
    }
    if (tool == "update_bot_settings") {
        if (!policy.allows(actor,Action::bot_settings,bot)) throw AccessDenied("Bot settings access denied");
        const auto id = argument(args,"environmentId");
        if (taskEnvironment(bot,routing,id) != id || policy.forbidden(bot,Action::environment_configure,id))
            throw Conflict("Environment configure access required");
        if (!args.contains("runTimeoutSeconds") || !args.at("runTimeoutSeconds").is_number_integer()) throw Conflict("Integer timeout required");
        const auto timeout = args.at("runTimeoutSeconds").get<Id>();
        if (timeout<1 || timeout>2147483647) throw Conflict("Invalid timeout");
        exec("INSERT INTO environment_settings VALUES(?,?,?) ON CONFLICT(bot,environment) DO UPDATE SET timeout=excluded.timeout",
             {bot,id,timeout});
        return {{"updated",true}};
    }
    return nullptr;
}
}
