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
    for (const auto& id : routing.at("config").value("conversationEnvironments",Json::object()))
        ids.insert(id.get<std::string>());
    return ids;
}
}

Seconds Store::Impl::environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const {
    auto row = query("SELECT timeout FROM environment_settings WHERE bot=? AND environment=?",{bot,environment});
    return row.next() ? row.number(0) : config.value("runTimeoutSeconds",Seconds{1800});
}

Json Store::Impl::readiness(std::string_view bot, const Json& routing, const Json& args, T3& t3) {
    Access access(*this);
    const auto& config = routing.at("config");
    const auto environment = field(routing,"environmentId");
    auto id = args.contains("environmentId") ? argument(args,"environmentId") : environment;
    auto settings = "/settings/"+std::string(bot)+"/environment";
    auto failure = [&](std::string reason) -> Json {return {{"ready",false},{"reason",reason},{"settingsLink",settings}};};
    if (!environments(routing).contains(id) || field(routing,"projectId").empty()) return failure("Configure the environment and project");
    if (!access.permits(bot,{"environment",id,"launch"},true)) return failure("Approve environment launch access");
    Command command;
    command.ownerBotId = bot;
    command.environmentId = id;
    command.projectId = field(routing,"projectId");
    command.config = config;
    command.actionId = actionId();
    command.tool = "delegate";
    command.instruction = "Check environment readiness";
    try {
        if (t3.authGeneration(id,config).empty() || t3.preflight(command)) return failure("Check T3 sign-in, project and connection permissions");
    } catch (...) { return failure("Check T3 sign-in and environment settings"); }
    return {{"ready",true}};
}

Json Store::Impl::accessTool(std::string_view bot, Id sourceMessage, const Json& routing, const Decision& decision) {
    Access access(*this);
    const auto& tool = decision.tool;
    const auto& args = decision.args;
    const auto& config = routing.at("config");
    if (tool == "list_grants") return {{"grants",access.grants(bot)}};
    if (tool == "request_access") {
        auto id = access.request(bot,{argument(args,"resourceKind"),argument(args,"resourceId"),argument(args,"operation")},sourceMessage);
        return {{"requestId",id},{"status","pending"}};
    }
    if (tool == "list_secrets") {
        auto rows = query(R"SQL(
SELECT name FROM protected_inputs p WHERE status='fulfilled' AND (bot=? OR EXISTS
 (SELECT 1 FROM bot_grants g WHERE g.bot=? AND g.kind='secret' AND g.resource=p.name AND g.op='use')) ORDER BY name
)SQL",{bot,bot});
        auto names = Json::array();
        while (rows.next()) names.push_back(rows.string(0));
        return {{"names",names}};
    }
    if (tool == "request_secret_input") {
        auto id = access.requestSecret(bot,{argument(args,"name")},sourceMessage);
        return {{"requestId",id},{"status","pending"},{"settingsLink","/settings/"+std::string(bot)+"/secrets"}};
    }
    if (tool == "list_connections") {
        Json result{{"environments",Json::array()},{"services",config.value("services",Json::array())},
                    {"mcpServers",config.value("mcpServers",Json::array())}};
        for (const auto& id : environments(routing)) {
            Json entry{{"environmentId",id}};
            if (access.permits(bot,{"environment",id,"inspect"},true))
                entry["runTimeoutSeconds"] = environmentTimeout(bot,id,config);
            result["environments"].push_back(entry);
        }
        return result;
    }
    if (tool == "update_bot_settings") {
        const auto id = argument(args,"environmentId");
        if (!environments(routing).contains(id) || !access.permits(bot,{"environment",id,"configure"},true)) throw Conflict("Environment configure access required");
        if (!args.contains("runTimeoutSeconds") || !args.at("runTimeoutSeconds").is_number_integer()) throw Conflict("Integer timeout required");
        const auto timeout = args.at("runTimeoutSeconds").get<Id>();
        if (timeout<1 || timeout>2147483647) throw Conflict("Invalid timeout");
        exec("INSERT INTO environment_settings VALUES(?,?,?) ON CONFLICT(bot,environment) DO UPDATE SET timeout=excluded.timeout",
             {bot,id,timeout});
        return {{"updated",true}};
    }
    if (tool == "request_bot_change") {
        const auto operation = argument(args,"operation"), target = argument(args,"targetBotId"), name = argument(args,"name");
        auto id = access.requestBotChange(bot,operation,target,name,sourceMessage);
        return {{"requestId",id},{"status","pending"}};
    }
    return nullptr;
}
}
