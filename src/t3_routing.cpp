#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
std::string conversationEnvironmentId(const Json& alias) {
    return alias.is_string() ? alias.get<std::string>() : field(alias,"environmentId");
}

std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name) {
    const auto& config = routing.at("config");
    const auto aliases = config.find("conversationEnvironments");
    const bool hasAliases = aliases != config.end() && aliases->is_object();
    if (hasAliases) {
        if (const auto alias = aliases->find(name); alias != aliases->end() && conversationEnvironmentId(*alias) == environment)
            if (auto project = field(*alias,"projectId"); !project.empty()) return project;
    }
    if (environment == field(routing,"environmentId"))
        if (auto project = field(routing,"projectId"); !project.empty()) return project;
    if (!hasAliases) return {};
    std::string selected;
    for (const auto& alias : *aliases) {
        if (conversationEnvironmentId(alias) != environment) continue;
        auto project = field(alias,"projectId");
        if (project.empty()) continue;
        if (!selected.empty() && selected != project) return {};
        selected = std::move(project);
    }
    return selected;
}

namespace {
std::string resolveAlias(const Json& config, std::string_view name) {
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    const auto canonical = std::ranges::any_of(*aliases,[&](const Json& value) {
        return conversationEnvironmentId(value) == name;
    });
    auto id = std::string(name);
    if (!canonical) {
        const auto alias = aliases->find(name);
        if (alias == aliases->end()) return {};
        id = conversationEnvironmentId(*alias);
    }
    constexpr auto valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.";
    if (id.size() > 1024 || id.find_first_not_of(valid) != std::string::npos) return {};
    return id;
}
}

std::set<std::string> Store::Impl::registeredT3Environments(std::string_view bot) {
    std::set<std::string> result;
    Policy policy(*this);
    auto rows = query("SELECT environment_id FROM t3_connected_environments");
    while (rows.next()) {
        auto id = rows.string(0);
        if (policy.allows(bot,Action::environment_inspect,id) && !policy.forbidden(bot,Action::environment_launch,id))
            result.insert(std::move(id));
    }
    return result;
}

std::string Store::Impl::defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred) {
    if (!configured.empty()) return std::string(configured);
    const auto connected = registeredT3Environments(bot);
    if (connected.contains(std::string(preferred))) return std::string(preferred);
    return connected.size() == 1 ? *connected.begin() : std::string{};
}

std::string Store::Impl::taskEnvironment(std::string_view bot, const Json& routing, std::string_view name,
                                        const Json* movingTask) {
    const auto configured = field(routing,"environmentId");
    if (name.empty())
        return defaultT3Environment(bot,configured,field(routing.value("sessionOrigin",Json::object()),"environmentId"));
    if (name == configured || (movingTask && name == field(*movingTask,"environmentId")))
        return std::string(name);
    if (movingTask && movingTask->at("projectId") != routing.at("projectId"))
        if (auto original = resolveAlias(movingTask->at("config"),name); !original.empty()) return original;
    if (auto alias = resolveAlias(routing.at("config"),name); !alias.empty()) return alias;
    if (configured.empty() && registeredT3Environments(bot).contains(std::string(name))) return std::string(name);
    return {};
}

}
