#include "storage.hpp"
#include "flock/limits.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <tuple>

namespace flockv0 {
std::string conversationEnvironmentId(const Json& alias) {
    return alias.is_string() ? alias.get<std::string>() : field(alias, "environmentId");
}
std::string conversationGatewayEnvironmentId(const Json& alias) {
    return field(alias, "gatewayEnvironmentId");
}

namespace {
std::string namedT3Project(const Json& config, std::string_view environment, std::string_view name) {
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    const auto alias = aliases->find(name);
    if (alias == aliases->end() || conversationEnvironmentId(*alias) != environment) return {};
    return field(*alias, "projectId");
}
}

std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name) {
    const auto& config = routing.at("config");
    if (auto project = namedT3Project(config, environment, name); !project.empty()) return project;
    if (environment == field(routing, "environmentId"))
        if (auto project = field(routing, "projectId"); !project.empty()) return project;
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    std::string selected;
    for (const auto& alias : *aliases) {
        if (conversationEnvironmentId(alias) != environment) continue;
        auto project = field(alias, "projectId");
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
    const auto canonical = std::ranges::any_of(*aliases, [&](const Json& value) { return conversationEnvironmentId(value) == name; });
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

std::optional<TaskRoute> configuredTaskRoute(const Json& config, std::string_view name) {
    const auto environment = resolveAlias(config, name);
    if (environment.empty()) return std::nullopt;
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return TaskRoute{environment, {}, {}};

    const std::string canonicalId(name);
    bool canonical = false;
    std::string canonicalGateway;
    std::string canonicalProject;
    for (const auto& alias : *aliases) {
        if (conversationEnvironmentId(alias) != name) continue;
        auto gateway = conversationGatewayEnvironmentId(alias);
        if (gateway.empty()) gateway = canonicalId;
        if (canonical && gateway != canonicalGateway) return TaskRoute{};
        canonical = true;
        canonicalGateway = std::move(gateway);
        auto project = field(alias, "projectId");
        if (!project.empty()) {
            if (!canonicalProject.empty() && project != canonicalProject) return TaskRoute{};
            canonicalProject = std::move(project);
        }
    }
    if (canonical) {
        if (canonicalGateway == canonicalId) canonicalGateway.clear();
        return TaskRoute{canonicalId, std::move(canonicalProject), std::move(canonicalGateway)};
    }
    const auto selected = aliases->find(name);
    if (selected != aliases->end())
        return TaskRoute{conversationEnvironmentId(*selected), field(*selected, "projectId"), conversationGatewayEnvironmentId(*selected)};
    return TaskRoute{environment, {}, {}};
}

std::string verifiedCommit(std::string commit) {
    if (commit.empty()) return commit;
    if ((commit.size() != 40 && commit.size() != 64) || commit.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw conversation_storage::ToolError("Supply a full commit SHA to verify");
    std::ranges::transform(commit, commit.begin(), [](char ch) { return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch; });
    return commit;
}
}

std::set<std::string> Store::Impl::registeredT3Environments(std::string_view bot) {
    std::set<std::string> result;
    Policy policy(*this);
    auto rows = query("SELECT environment_id FROM t3_connected_environments");
    while (rows.next()) {
        auto id = rows.string(0);
        if (policy.allows(bot, Action::environment_inspect, id) && !policy.forbidden(bot, Action::environment_launch, id))
            result.insert(std::move(id));
    }
    return result;
}

LinkedTaskDestinations Store::Impl::discoverLinkedTaskDestinations(T3& t3, std::string_view bot) {
    std::set<std::string> gateways;
    {
        std::lock_guard lock(mutex);
        gateways = registeredT3Environments(bot);
    }
    LinkedTaskDestinations result;
    const auto scrub = redactor(bot);
    std::set<std::tuple<std::string, std::string, std::string>> seenRoutes;
    std::set<std::string> seenNames;
    for (const auto& gateway : gateways) {
        T3EnvironmentLinks links;
        try {
            links = t3.linkedEnvironments(gateway);
        } catch (const std::exception&) {
            links.unavailable = "Flock could not list linked environments through T3 gateway " + gateway + ".";
        }
        if (!links.unavailable.empty()) {
            result.unavailable.push_back(scrub.text(links.unavailable));
            continue;
        }
        for (const auto& link : links.links) {
            const auto& target = link.route.environmentId;
            if (link.route.gatewayEnvironmentId != gateway || target.empty()) {
                result.unavailable.push_back(scrub.text("T3 returned an invalid linked-environment identity for gateway " + gateway + "."));
                continue;
            }
            if (link.status != T3EnvironmentLink::Status::reachable) {
                const auto status = link.status == T3EnvironmentLink::Status::expired ? "expired" : "unreachable";
                result.unavailable.push_back(scrub.text("Linked target " + link.label + " via " + gateway + " is " + status + "."));
                continue;
            }
            bool permitted = false;
            {
                std::lock_guard lock(mutex);
                Policy policy(*this);
                permitted =
                    policy.allows(bot, Action::environment_inspect, target) && !policy.forbidden(bot, Action::environment_launch, target);
            }
            if (!permitted) continue;
            try {
                auto projects = t3.projects(link.route);
                if (projects.empty()) {
                    result.unavailable.push_back(
                        scrub.text("No available T3 projects were found for linked target " + link.label + " via " + gateway + "."));
                    continue;
                }
                for (const auto& project : projects) {
                    if (project.id.empty() || project.title.empty() || !seenRoutes.emplace(target, project.id, gateway).second) continue;
                    scrub.requirePublic({target, project.id, gateway});
                    const auto identity = " (" + target + " via " + gateway + ")";
                    const auto projectLabel = " — " + scrub.text(project.title) + " (" + project.id + ")";
                    if (identity.size() + projectLabel.size() >= 256) {
                        result.unavailable.push_back("A linked destination identity is too long to select safely.");
                        continue;
                    }
                    const auto label = bounded(scrub.text(link.label), 256 - identity.size() - projectLabel.size());
                    const auto name = label + identity + projectLabel;
                    if (!seenNames.insert(name).second) continue;
                    result.choices.push_back({name, {target, project.id, gateway}});
                }
            } catch (const std::exception&) {
                result.unavailable.push_back(scrub.text("Flock could not list projects for linked target " + link.label +
                                                        " through gateway " + gateway + ". Check linked-environment access in T3."));
            }
        }
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
    return taskRoute(bot, routing, name).environmentId;
}

TaskRoute Store::Impl::taskRoute(std::string_view bot, const Json& routing, std::string_view name, const TaskData* movingTask,
                                 const LinkedTaskDestinations* linked) {
    using conversation_storage::ToolError;
    const auto configured = field(routing, "environmentId");
    const auto resolved = [&](TaskRoute route) {
        if (route.environmentId.empty()) return route;
        if (route.projectId.empty() && movingTask) {
            route.projectId = namedT3Project(routing.at("config"), route.environmentId, name);
            if (route.projectId.empty()) {
                if (route.environmentId == movingTask->environmentId)
                    route.projectId = movingTask->projectId;
                else if (route.environmentId == configured)
                    route.projectId = field(routing, "projectId");
            }
        } else if (route.projectId.empty())
            route.projectId = configuredT3Project(routing, route.environmentId, name);
        if (route.gatewayEnvironmentId.empty()) route.gatewayEnvironmentId = route.environmentId;
        return route;
    };
    const auto connected = [&](const TaskRoute& route) {
        return route.gatewayEnvironmentId == route.environmentId || registeredT3Environments(bot).contains(route.gatewayEnvironmentId);
    };
    if (name.empty())
        return resolved(
            {defaultT3Environment(bot, configured, field(routing.value("sessionOrigin", Json::object()), "environmentId")), {}, {}});
    if (movingTask && name == movingTask->environmentId) {
        const auto route = resolved({std::string(name), {}, movingTask->gatewayEnvironmentId.value_or("")});
        return connected(route) ? route : TaskRoute{};
    }
    // The Bot's default ID is the direct route offered by change_task.
    if (movingTask && name == configured) return resolved({std::string(name), {}, {}});
    std::optional<TaskRoute> route;
    if (name == configured)
        route = TaskRoute{std::string(name), {}, {}};
    else {
        if (movingTask && movingTask->projectId != field(routing, "projectId")) route = configuredTaskRoute(movingTask->config, name);
        if (!route) route = configuredTaskRoute(routing.at("config"), name);
        if (!route && configured.empty() && registeredT3Environments(bot).contains(std::string(name)))
            route = TaskRoute{std::string(name), {}, {}};
    }
    if (route && route->environmentId.empty()) {
        if (linked) throw ToolError("This T3 environment has conflicting configured routes. Choose a specific destination name.");
        return {};
    }
    if (route) *route = resolved(std::move(*route));
    const TaskRoute* discovered = nullptr;
    if (linked) {
        for (const auto& choice : linked->choices) {
            if (choice.name != name && choice.route.environmentId != name) continue;
            if (discovered) throw ToolError("This T3 environment has more than one linked route. Choose a specific destination name.");
            discovered = &choice.route;
        }
    }
    if (!discovered) return route && connected(*route) ? *route : TaskRoute{};
    if (route && (route->environmentId != discovered->environmentId || route->projectId != discovered->projectId ||
                  route->gatewayEnvironmentId != discovered->gatewayEnvironmentId))
        throw ToolError(
            "This destination name resolves to both a configured route and a linked T3 gateway. Choose an unambiguous destination.");
    if (!registeredT3Environments(bot).contains(discovered->gatewayEnvironmentId))
        throw ToolError("The selected T3 gateway is no longer connected. Flock will not switch gateways.");
    Policy policy(*this);
    if (!policy.allows(bot, Action::environment_inspect, discovered->environmentId) ||
        policy.forbidden(bot, Action::environment_launch, discovered->environmentId))
        throw ToolError("Flock cannot launch in the linked target under the current permissions.");
    return *discovered;
}

StartDestinations Store::Impl::startDestinations(const conversation::Message& message, const Json& args, bool resume,
                                                 const WorkSession* selectedSession, const LinkedTaskDestinations& linked) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    const auto environment = text(args, "environment", 256, false);
    const auto sessionRef = text(args, "sessionRef", 128, false);
    StartDestinations destinations;
    const auto routeForName = [&](std::string_view name) { return taskRoute(bot, routing, name, nullptr, &linked); };
    destinations.commit = verifiedCommit(text(args, "commit", 64, false));
    if (resume && !destinations.commit.empty())
        throw ToolError("Commit verification is not supported when resuming a cancelled task; start a new task on its saved destination.");
    if (const auto environments = args.find("environments"); environments != args.end()) {
        if (!message.channelScope.empty() || routing.contains("sessionOrigin"))
            throw ToolError("Start work across T3 environments from a Flock conversation");
        if (!environment.empty() || !sessionRef.empty() || selectedSession || resume)
            throw ToolError("Choose either multiple environments or a single destination for this request");
        if (!environments->is_array() || environments->empty()) throw ToolError("Choose one or more names or IDs from task.environments");
        std::set<std::pair<std::string, std::string>> selected;
        for (const auto& choice : *environments) {
            const auto name = text(Json{{"environment", choice}}, "environment", 256, true);
            auto route = routeForName(name);
            if (route.environmentId.empty())
                throw ToolError(
                    linked.unavailable.empty()
                        ? "Choose names or IDs from task.environments"
                        : "The linked destination is unavailable through its T3 gateway. Check the link and gateway; Flock will not use a different environment.");
            if (route.projectId.empty()) route.projectId = rememberedT3Project(bot, route.environmentId);
            if (route.projectId.empty())
                throw ToolError("Choose a T3 project for " + name + " first, or configure its projectId in conversationEnvironments");
            if (!selected.emplace(route.environmentId, route.projectId).second)
                throw ToolError("Choose each environment and project only once");
            auto target = Json{{"environmentId", route.environmentId}, {"projectId", route.projectId}, {"name", name}};
            if (!route.gatewayEnvironmentId.empty() && route.gatewayEnvironmentId != route.environmentId)
                target["gatewayEnvironmentId"] = route.gatewayEnvironmentId;
            destinations.targets.push_back(std::move(target));
            if (destinations.primary.environmentId.empty()) destinations.primary = std::move(route);
        }
        redactor(bot).requirePublic({destinations.targets, destinations.commit});
        return destinations;
    }
    if (!sessionRef.empty() && (!selectedSession || !permitsWorkSession(message, *selectedSession)))
        throw ToolError("The selected T3 Session is unavailable. Search for it again and check its connection.");
    if (selectedSession) {
        if (!environment.empty() || resume) throw ToolError("A selected T3 Session fixes the destination. Omit environment and resume.");
        destinations.primary = {selectedSession->environmentId, selectedSession->projectId, {}};
    } else
        destinations.primary = environment.empty() ? taskRoute(bot, routing, environment) : routeForName(environment);
    if (destinations.primary.environmentId.empty())
        throw ToolError(
            environment.empty()
                ? "Choose a connected T3 environment to start work. Use task.environments or connect T3 in Settings."
                : (linked.unavailable.empty()
                       ? "Unknown environment " + environment + "; use one of task.environments"
                       : "The linked destination is unavailable through its T3 gateway. Check the link and gateway; Flock will not use a different environment."));
    if (!destinations.commit.empty()) {
        if (destinations.primary.projectId.empty())
            destinations.primary.projectId = rememberedT3Project(bot, destinations.primary.environmentId);
        if (destinations.primary.projectId.empty()) throw ToolError("Choose a T3 project before verifying a commit");
        auto target = Json{{"environmentId", destinations.primary.environmentId},
                           {"projectId", destinations.primary.projectId},
                           {"name", environment.empty() ? destinations.primary.environmentId : environment}};
        if (!destinations.primary.gatewayEnvironmentId.empty() &&
            destinations.primary.gatewayEnvironmentId != destinations.primary.environmentId)
            target["gatewayEnvironmentId"] = destinations.primary.gatewayEnvironmentId;
        destinations.targets.push_back(std::move(target));
        redactor(bot).requirePublic({destinations.targets, destinations.commit});
    }
    return destinations;
}

TaskChange Store::Impl::taskChange(const conversation::Message& message, const Json& args, const TaskData& task,
                                   const LinkedTaskDestinations& linked) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    using limits::session_prompt::maxDecisionTextBytes;
    TaskChange change{text(args, "intent", maxDecisionTextBytes, false), text(args, "doneWhen", maxDecisionTextBytes, false), {}};
    const auto environment = text(args, "environment", 256, false);
    if (change.intent.empty() && change.doneWhen.empty() && environment.empty())
        throw ToolError("change_task needs intent, doneWhen, or environment; use send_to_task for other instructions");
    if (!environment.empty()) {
        change.route = taskRoute(message.bot, message.routing, environment, &task, &linked);
        if (change.route.environmentId.empty()) throw ToolError("Unknown environment " + environment + "; use one of task.environments");
        redactor(message.bot).requirePublic({change.route.environmentId, change.route.projectId, change.route.gatewayEnvironmentId});
    }
    return change;
}

}
