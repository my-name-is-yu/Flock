#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>

namespace flockv0 {
std::string conversationEnvironmentId(const Json& alias) {
    return alias.is_string() ? alias.get<std::string>() : field(alias,"environmentId");
}

namespace {
std::string namedT3Project(const Json& config, std::string_view environment, std::string_view name) {
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    const auto alias = aliases->find(name);
    if (alias == aliases->end() || conversationEnvironmentId(*alias) != environment) return {};
    return field(*alias,"projectId");
}
}

std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name) {
    const auto& config = routing.at("config");
    if (auto project = namedT3Project(config,environment,name); !project.empty()) return project;
    if (environment == field(routing,"environmentId"))
        if (auto project = field(routing,"projectId"); !project.empty()) return project;
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
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

std::string verifiedCommit(std::string commit) {
    if (commit.empty()) return commit;
    if ((commit.size() != 40 && commit.size() != 64) ||
        commit.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw conversation_storage::ToolError("Supply a full commit SHA to verify");
    std::ranges::transform(commit,commit.begin(),[](char ch) {
        return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch;
    });
    return commit;
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

std::string Store::Impl::taskEnvironment(std::string_view bot, const Json& routing, std::string_view name) {
    return taskRoute(bot,routing,name).environmentId;
}

TaskRoute Store::Impl::taskRoute(std::string_view bot, const Json& routing, std::string_view name,
                                const Json* movingTask) {
    const auto configured = field(routing,"environmentId");
    const auto resolved = [&](std::string environment, std::string project = {}) -> TaskRoute {
        if (environment.empty()) return {};
        if (project.empty() && movingTask) {
            project = namedT3Project(routing.at("config"),environment,name);
            if (project.empty()) {
                if (environment == field(*movingTask,"environmentId")) project = field(*movingTask,"projectId");
                else if (environment == configured) project = field(routing,"projectId");
            }
        } else if (project.empty()) project = configuredT3Project(routing,environment,name);
        return {std::move(environment),std::move(project)};
    };
    if (name.empty()) return resolved(defaultT3Environment(bot,configured,
        field(routing.value("sessionOrigin",Json::object()),"environmentId")));
    if (name == configured || (movingTask && name == field(*movingTask,"environmentId")))
        return resolved(std::string(name));
    if (movingTask && movingTask->at("projectId") != routing.at("projectId")) {
        const auto& config = movingTask->at("config");
        if (auto original = resolveAlias(config,name); !original.empty())
            return resolved(original,namedT3Project(config,original,name));
    }
    if (auto alias = resolveAlias(routing.at("config"),name); !alias.empty()) return resolved(std::move(alias));
    if (configured.empty() && registeredT3Environments(bot).contains(std::string(name))) return resolved(std::string(name));
    return {};
}

StartDestinations Store::Impl::startDestinations(const conversation::Message& message, const Json& args, bool resume,
                                                const WorkSession* selectedSession) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    const auto environment = text(args,"environment",256,false);
    const auto sessionRef = text(args,"sessionRef",128,false);
    StartDestinations destinations;
    destinations.commit = verifiedCommit(text(args,"commit",64,false));
    if (resume && !destinations.commit.empty())
        throw ToolError("Commit verification is not supported when resuming a cancelled task; start a new task on its saved destination.");
    if (const auto environments = args.find("environments"); environments != args.end()) {
        if (!message.channelScope.empty() || routing.contains("sessionOrigin"))
            throw ToolError("Start work across T3 environments from a Flock conversation");
        if (!environment.empty() || !sessionRef.empty() || selectedSession || resume)
            throw ToolError("Choose either multiple environments or a single destination for this request");
        if (!environments->is_array() || environments->empty() || environments->size() > 16)
            throw ToolError("Choose one or more names or IDs from task.environments");
        std::set<std::pair<std::string,std::string>> selected;
        for (const auto& choice : *environments) {
            const auto name = text(Json{{"environment",choice}},"environment",256,true);
            auto route = taskRoute(bot,routing,name);
            if (route.environmentId.empty())
                throw ToolError("Choose names or IDs from task.environments");
            if (route.projectId.empty()) route.projectId = rememberedT3Project(bot,route.environmentId);
            if (route.projectId.empty())
                throw ToolError("Choose a T3 project for " + name +
                                " first, or configure its projectId in conversationEnvironments");
            if (!selected.emplace(route.environmentId,route.projectId).second)
                throw ToolError("Choose each environment and project only once");
            destinations.targets.push_back({{"environmentId",route.environmentId},{"projectId",route.projectId},{"name",name}});
            if (destinations.primary.environmentId.empty()) destinations.primary = std::move(route);
        }
        redactor(bot).requirePublic({destinations.targets,destinations.commit});
        return destinations;
    }
    if (!sessionRef.empty() && (!selectedSession || !permitsWorkSession(message,*selectedSession)))
        throw ToolError("The selected T3 Session is unavailable. Search for it again and check its connection.");
    if (selectedSession) {
        if (!environment.empty() || resume)
            throw ToolError("A selected T3 Session fixes the destination. Omit environment and resume.");
        destinations.primary = {selectedSession->environmentId,selectedSession->projectId};
    } else destinations.primary = taskRoute(bot,routing,environment);
    if (destinations.primary.environmentId.empty())
        throw ToolError(environment.empty()
            ? "Choose a connected T3 environment to start work. Use task.environments or connect T3 in Settings."
            : "Unknown environment " + environment + "; use one of task.environments");
    if (!destinations.commit.empty()) {
        if (destinations.primary.projectId.empty())
            destinations.primary.projectId = rememberedT3Project(bot,destinations.primary.environmentId);
        if (destinations.primary.projectId.empty())
            throw ToolError("Choose a T3 project before verifying a commit");
        destinations.targets.push_back({{"environmentId",destinations.primary.environmentId},
            {"projectId",destinations.primary.projectId},{"name",environment.empty() ? destinations.primary.environmentId : environment}});
        redactor(bot).requirePublic({destinations.targets,destinations.commit});
    }
    return destinations;
}

TaskChange Store::Impl::taskChange(const conversation::Message& message, const Json& args, const Json& task) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    TaskChange change{text(args,"intent",8000,false),text(args,"doneWhen",8000,false),{}};
    const auto environment = text(args,"environment",256,false);
    if (change.intent.empty() && change.doneWhen.empty() && environment.empty())
        throw ToolError("change_task needs intent, doneWhen, or environment; use send_to_task for other instructions");
    if (!environment.empty()) {
        change.route = taskRoute(message.bot,message.routing,environment,&task);
        if (change.route.environmentId.empty())
            throw ToolError("Unknown environment " + environment + "; use one of task.environments");
        redactor(message.bot).requirePublic({change.route.environmentId,change.route.projectId});
    }
    return change;
}

}
