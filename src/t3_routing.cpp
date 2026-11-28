#include "task_routing.hpp"
#include "flock/digest.hpp"
#include "flock/limits.hpp"
#include "conversation_storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/task.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "session_read_storage.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr std::size_t maxEnvironmentSelectorBytes = 256;

std::string linkedSelector(std::string_view label, std::string_view title, const TaskRoute& route) {
    constexpr std::string_view separator = " — ";
    const auto key = Json::array({route.environmentId, route.projectId, route.gatewayEnvironmentId}).dump();
    const auto selector = " [" + sha256Hex(key).substr(0, 24) + "]";
    const auto identity = " (" + route.environmentId + " via " + route.gatewayEnvironmentId + ")";
    const auto project = " (" + route.projectId + ")";
    const auto remaining = maxEnvironmentSelectorBytes - selector.size();
    const auto fixedBytes = identity.size() + separator.size() + project.size();
    if (fixedBytes > remaining) return bounded(title, remaining) + selector;

    const auto shortTitle = bounded(title, remaining - fixedBytes);
    auto name = bounded(label, remaining - fixedBytes - shortTitle.size()) + identity;
    return name.append(separator).append(shortTitle).append(project).append(selector);
}

std::string verifiedCommit(std::string commit) {
    if (commit.empty()) return commit;
    if ((commit.size() != 40 && commit.size() != 64) || commit.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw conversation_storage::ToolError("Supply a full commit SHA to verify");
    std::ranges::transform(commit, commit.begin(), [](char ch) { return ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch; });
    return commit;
}
}

std::set<std::string> TaskRouting::registeredT3Environments(std::string_view bot) {
    std::set<std::string> result;
    auto& policy = permissions;
    auto rows = database.query("SELECT environment_id FROM t3_connected_environments");
    while (rows.next()) {
        auto id = rows.string(0);
        if (policy.allows(bot, Action::environment_inspect, id) && !policy.forbidden(bot, Action::environment_launch, id))
            result.insert(std::move(id));
    }
    return result;
}

namespace {
auto catalogKey(const TaskRoute& route) {
    return std::pair{route.environmentId, route.gatewayEnvironmentId.empty() ? route.environmentId : route.gatewayEnvironmentId};
}
}

const std::vector<T3Project>* TaskDestinations::projects(const TaskRoute& route) const {
    const auto catalog = catalogs.find(catalogKey(route));
    return catalog == catalogs.end() ? nullptr : &catalog->second;
}

void TaskDestinations::chooseProject(TaskRoute& route, std::string_view project) const {
    if (project.empty()) return;
    const auto* listed = projects(route);
    if ((!listed || std::ranges::find(*listed, project, &T3Project::id) == listed->end()) &&
        !std::ranges::any_of(linked, [&](const RunTarget& choice) {
            return catalogKey(choice.route) == catalogKey(route) && choice.route.projectId == project;
        }))
        throw conversation_storage::ToolError("Choose a listed projectId for the selected T3 environment.");
    route.projectId = project;
}

TaskDestinations TaskRouting::discoverTaskDestinations(T3& t3, const conversation::Message& message, bool listProjects,
                                                       const Json* change) {
    const auto& bot = message.bot;
    std::set<std::string> gateways;
    {
        std::lock_guard lock(database.mutex);
        gateways = registeredT3Environments(bot);
    }
    TaskDestinations result;
    const auto scrub = permissions.secretRedactor(bot);
    std::set<std::tuple<std::string, std::string, std::string>> seenRoutes;
    for (const auto& gateway : gateways) {
        T3EnvironmentLinks links;
        try {
            links = t3.linkedEnvironments(gateway);
        } catch (const std::exception&) {
            links.unavailable = "Flock couldn’t list linked environments through T3 gateway " + gateway + ".";
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
                std::lock_guard lock(database.mutex);
                auto& policy = permissions;
                permitted =
                    policy.allows(bot, Action::environment_inspect, target) && !policy.forbidden(bot, Action::environment_launch, target);
            }
            if (!permitted) continue;
            if (!link.access.empty()) result.linkAccess[{target, gateway}] = scrub.text(link.access);
            try {
                auto projects = t3.projects(link.route);
                if (projects.empty()) {
                    result.unavailable.push_back(
                        scrub.text("No available T3 projects were found for linked target " + link.label + " via " + gateway + "."));
                    continue;
                }
                for (const auto& project : projects) {
                    if (project.id.empty() || !seenRoutes.emplace(target, project.id, gateway).second) continue;
                    scrub.requirePublic({target, project.id, gateway});
                    TaskRoute route{target, project.id, gateway};
                    auto name = linkedSelector(scrub.text(link.label), scrub.text(project.title), route);
                    result.linked.push_back({std::move(name), std::move(route)});
                }
            } catch (const std::exception&) {
                result.unavailable.push_back(scrub.text("Flock couldn’t list projects for linked target " + link.label +
                                                        " through gateway " + gateway + ". Check linked-environment access in T3."));
            }
        }
    }
    if (!listProjects && (!change || field(*change, "environment").empty())) return result;

    std::map<std::pair<std::string, std::string>, TaskRoute> routes;
    {
        std::lock_guard lock(database.mutex);
        const auto& routing = message.routing;
        const auto add = [&](TaskRoute route) {
            if (route.environmentId.empty()) return;
            auto key = catalogKey(route);
            routes.try_emplace(std::move(key), std::move(route));
        };
        if (!change) {
            const auto defaultRoute = taskRoute(bot, routing, {});
            if (!defaultRoute.environmentId.empty()) {
                result.direct.push_back({{}, defaultRoute});
                add(defaultRoute);
            }
            for (const auto& gateway : gateways) {
                if (gateway == defaultRoute.environmentId) continue;
                TaskRoute route{gateway, {}, gateway};
                result.direct.push_back({gateway, route});
                add(std::move(route));
            }
        }
        if (change) {
            const auto id = change->find("taskId");
            if (id != change->end() && id->is_number_integer()) {
                auto row = database.query("SELECT environment_id,project_id,data FROM tasks WHERE id=? AND bot_id=?", {*id, bot});
                if (row.next()) {
                    const auto data = row.json(2);
                    TaskData task{.environmentId = row.string(0), .projectId = row.string(1)};
                    if (data.contains("gatewayEnvironmentId")) task.gatewayEnvironmentId = field(data, "gatewayEnvironmentId");
                    const auto name = field(*change, "environment");
                    if (name.empty())
                        add({task.environmentId, task.projectId, task.gatewayEnvironmentId.value_or("")});
                    else
                        add(taskRoute(bot, routing, name, &task));
                }
            }
        }
    }
    for (const auto& [key, route] : routes) {
        auto catalog = readProjectCatalog(t3, bot, route, false);
        if (auto* projects = std::get_if<std::vector<T3Project>>(&catalog)) result.catalogs.emplace(key, std::move(*projects));
    }
    return result;
}

std::string TaskRouting::defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred) {
    if (!configured.empty()) return std::string(configured);
    const auto connected = registeredT3Environments(bot);
    if (connected.contains(std::string(preferred))) return std::string(preferred);
    return connected.size() == 1 ? *connected.begin() : std::string{};
}

std::string TaskRouting::taskEnvironment(std::string_view bot, const Json& routing, std::string_view name) {
    return taskRoute(bot, routing, name).environmentId;
}

TaskRoute TaskRouting::taskRoute(std::string_view bot, const Json& routing, std::string_view name, const TaskData* movingTask,
                                 const TaskDestinations* linked) {
    using conversation_storage::ToolError;
    const auto configured = field(routing, "environmentId");
    const auto directRoute = [&](std::string environment) {
        auto project = environment == configured ? field(routing, "projectId") : std::string{};
        return TaskRoute{environment, std::move(project), environment};
    };
    if (name.empty())
        return directRoute(defaultT3Environment(bot, configured, field(routing.value("sessionOrigin", Json::object()), "environmentId")));
    if (movingTask && name == movingTask->environmentId) {
        const auto gateway = movingTask->gatewayEnvironmentId.value_or(movingTask->environmentId);
        if (gateway != name && !registeredT3Environments(bot).contains(gateway)) return {};
        return {movingTask->environmentId, movingTask->projectId, gateway};
    }
    const auto connected = registeredT3Environments(bot);
    if (name == configured || connected.contains(std::string(name))) return directRoute(std::string(name));
    if (!linked) return {};

    const TaskRoute* discovered = nullptr;
    for (const auto& choice : linked->linked) {
        if (choice.name == name) {
            discovered = &choice.route;
            break;
        }
        if (choice.route.environmentId != name) continue;
        if (discovered) throw ToolError("This T3 environment has more than one linked route. Choose a specific destination name.");
        discovered = &choice.route;
    }
    if (!discovered) return {};
    if (!connected.contains(discovered->gatewayEnvironmentId))
        throw ToolError("The selected T3 gateway is no longer connected. Flock will not switch gateways.");
    auto& policy = permissions;
    if (!policy.allows(bot, Action::environment_inspect, discovered->environmentId) ||
        policy.forbidden(bot, Action::environment_launch, discovered->environmentId))
        throw ToolError("Flock cannot launch in the linked target under the current permissions.");
    return *discovered;
}

StartDestinations TaskRouting::startDestinations(const conversation::Message& message, const Json& args, bool resume,
                                                 const WorkSession* selectedSession, const TaskDestinations& linked) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    const auto environment = text(args, "environment", maxEnvironmentSelectorBytes, false);
    const auto sessionRef = text(args, "sessionRef", 128, false);
    const auto project = text(args, "projectId", 1024, args.contains("projectId"));
    StartDestinations destinations;
    const auto routeForName = [&](std::string_view name) { return taskRoute(bot, routing, name, nullptr, &linked); };
    const auto resolveProject = [&](TaskRoute& route, std::string_view name) {
        if (route.projectId.empty()) route.projectId = rememberedT3Project(bot, route.environmentId);
        if (route.projectId.empty())
            if (const auto* projects = linked.projects(route); projects && projects->size() == 1) route.projectId = projects->front().id;
        if (route.projectId.empty()) throw ToolError("Choose a listed projectId for " + std::string(name) + ".");
    };
    destinations.commit = verifiedCommit(text(args, "commit", 64, false));
    if (resume && !destinations.commit.empty())
        throw ToolError("Commit verification is not supported when resuming a cancelled task; start a new task on its saved destination.");
    if (const auto environments = args.find("environments"); environments != args.end()) {
        if (!message.channelScope.empty() || routing.contains("sessionOrigin"))
            throw ToolError("Start work across T3 environments from a Flock conversation");
        if (!environment.empty() || !project.empty() || !sessionRef.empty() || selectedSession || resume)
            throw ToolError("Choose either multiple environments or a single destination for this request");
        if (!environments->is_array() || environments->empty()) throw ToolError("Choose one or more names or IDs from task.environments");
        std::set<std::pair<std::string, std::string>> selected;
        for (const auto& choice : *environments) {
            const auto selection = choice.is_object() ? choice : Json{{"environment", choice}};
            if (selection.size() != (selection.contains("projectId") ? 2 : 1))
                throw ToolError("Use environment and projectId to select a destination.");
            const auto name = text(selection, "environment", maxEnvironmentSelectorBytes, true);
            const auto selectedProject = text(selection, "projectId", 1024, selection.contains("projectId"));
            auto route = routeForName(name);
            if (route.environmentId.empty())
                throw ToolError(
                    linked.unavailable.empty()
                        ? "Choose names or IDs from task.environments"
                        : "The linked destination is unavailable through its T3 gateway. Check the link and gateway; Flock will not use a different environment.");
            linked.chooseProject(route, selectedProject);
            resolveProject(route, name);
            if (!selected.emplace(route.environmentId, route.projectId).second)
                throw ToolError("Choose each environment and project only once");
            if (destinations.primary.environmentId.empty()) destinations.primary = route;
            destinations.targets.push_back({name, std::move(route)});
        }
        permissions.secretRedactor(bot).requirePublic({runTargetsJson(destinations.targets), destinations.commit});
        return destinations;
    }
    if (!sessionRef.empty() && (!selectedSession || !sessionReadStorage.permitsWorkSession(message, *selectedSession)))
        throw ToolError("The selected T3 Session is unavailable. Search for it again and check its connection.");
    if (selectedSession) {
        if (!environment.empty() || !project.empty() || resume)
            throw ToolError("A selected T3 Session fixes the destination. Omit environment, projectId, and resume.");
        destinations.primary = {selectedSession->target.environmentId, selectedSession->projectId, {}};
    } else
        destinations.primary = environment.empty() ? taskRoute(bot, routing, environment) : routeForName(environment);
    if (destinations.primary.environmentId.empty())
        throw ToolError(
            environment.empty()
                ? "Choose a connected T3 environment to start work. Use task.environments or connect T3 in Settings."
                : (linked.unavailable.empty()
                       ? "Unknown environment " + environment + "; use one of task.environments"
                       : "The linked destination is unavailable through its T3 gateway. Check the link and gateway; Flock will not use a different environment."));
    linked.chooseProject(destinations.primary, project);
    const auto& name = environment.empty() ? destinations.primary.environmentId : environment;
    if (!selectedSession) resolveProject(destinations.primary, name);
    if (!destinations.commit.empty()) {
        destinations.targets.push_back({name, destinations.primary});
        permissions.secretRedactor(bot).requirePublic({runTargetsJson(destinations.targets), destinations.commit});
    }
    return destinations;
}

TaskChange TaskRouting::taskChange(const conversation::Message& message, const Json& args, const TaskData& task,
                                   const TaskDestinations& linked) {
    using conversation_storage::text;
    using conversation_storage::ToolError;
    using limits::session_prompt::maxDecisionTextBytes;
    TaskChange change{text(args, "intent", maxDecisionTextBytes, false), text(args, "doneWhen", maxDecisionTextBytes, false), {}};
    const auto environment = text(args, "environment", maxEnvironmentSelectorBytes, false);
    const auto project = text(args, "projectId", 1024, args.contains("projectId"));
    if (change.intent.empty() && change.doneWhen.empty() && environment.empty() && project.empty())
        throw ToolError("change_task needs intent, doneWhen, environment, or projectId; use send_to_task for other instructions");
    if (!environment.empty() || !project.empty()) {
        change.route = environment.empty()
                           ? TaskRoute{task.environmentId, task.projectId, task.gatewayEnvironmentId.value_or(task.environmentId)}
                           : taskRoute(message.bot, message.routing, environment, &task, &linked);
        if (change.route.environmentId.empty()) throw ToolError("Unknown environment " + environment + "; use one of task.environments");
        linked.chooseProject(change.route, project);
        if (project.empty() && (change.route.environmentId != task.environmentId ||
                                change.route.gatewayEnvironmentId != task.gatewayEnvironmentId.value_or(task.environmentId) ||
                                change.route.projectId.empty())) {
            const auto* projects = linked.projects(change.route);
            if (projects) {
                change.route.projectId =
                    selectT3Project(*projects, change.route.projectId, rememberedT3Project(message.bot, change.route.environmentId));
                if (change.route.projectId.empty())
                    throw ToolError(projects->empty() ? "Add or unarchive a project in the selected T3 environment."
                                                      : "Choose a listed projectId for the selected T3 environment.",
                                    {{"projects", projectChoices(*projects, permissions.secretRedactor(message.bot))}});
            } else {
                // A named linked route already contains a project verified through its gateway.
                if (change.route.projectId.empty())
                    throw ToolError("Flock couldn’t list projects for the selected destination. Check its T3 connection.");
                linked.chooseProject(change.route, std::string(change.route.projectId));
            }
        }
        permissions.secretRedactor(message.bot)
            .requirePublic({change.route.environmentId, change.route.projectId, change.route.gatewayEnvironmentId});
    }
    return change;
}

}
