#include "wait_storage.hpp"
#include "task_routing.hpp"
#include "peer_storage.hpp"
#include "flock/types.hpp"
#include "flock/t3.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "session_participation_storage.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
std::string TaskRouting::rememberedT3Project(std::string_view bot, std::string_view environment) const {
    auto row = database.query("SELECT project_id FROM bot_t3_projects WHERE bot_id=? AND environment_id=?", {bot, environment});
    return row.next() ? row.string(0) : std::string{};
}

void TaskRouting::rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project) {
    database.exec("INSERT INTO bot_t3_projects VALUES(?,?,?) ON CONFLICT(bot_id,environment_id) "
                  "DO UPDATE SET project_id=excluded.project_id",
                  {bot, environment, project});
}

ProjectCatalog TaskRouting::readProjectCatalog(T3& t3, std::string_view bot, const TaskRoute& route, bool requireConnection) {
    const auto& environment = route.environmentId;
    const auto& gateway = route.gatewayEnvironmentId.empty() ? environment : route.gatewayEnvironmentId;
    {
        std::lock_guard lock(database.mutex);
        if (requireConnection && !sessionParticipationStorage.t3EnvironmentConnected(gateway))
            return "Connect this T3 environment under Connections in Flock settings.";
        if (!permissions.allows(bot, Action::environment_inspect, environment) ||
            permissions.forbidden(bot, Action::environment_launch, environment))
            return "Flock cannot use this T3 environment under the current permissions.";
    }
    try {
        auto projects = t3.projects(T3EnvironmentRoute{environment, gateway});
        const auto scrub = permissions.secretRedactor(bot);
        std::erase_if(projects, [&](const T3Project& project) { return project.id.empty() || scrub.text(project.id) != project.id; });
        return projects;
    } catch (const std::exception&) {
        return "Flock couldn’t list projects. Check the T3 connection in Flock settings.";
    }
}

Json projectChoices(const std::vector<T3Project>& projects, const SecretRedactor& scrub) {
    auto choices = Json::array();
    std::map<std::string, int> titles;
    for (const auto& project : projects) {
        auto title = bounded(scrub.text(project.title), 140);
        ++titles[title];
        choices.push_back({{"projectId", project.id}, {"title", std::move(title)}});
    }
    for (std::size_t i = 0; i < projects.size(); ++i) {
        auto& choice = choices.at(i);
        if (titles.at(field(choice, "title")) > 1 && !projects[i].directory.empty())
            choice["directory"] = bounded(scrub.text(projects[i].directory), 256);
    }
    return choices;
}

void TaskRouting::assignProject(TaskData& task, std::string_view project) {
    if (peerStorage.recipientRouteWouldCycle(task.id, task.environmentId, project))
        throw Conflict("This project would make project queues wait for one another. Choose another project.");
    task.projectId = project;
    rememberT3Project(task.botId, task.environmentId, project);
}

void TaskRouting::applyProjectCatalog(TaskData& task, const ProjectCatalog& catalog) {
    const auto& bot = task.botId;
    const auto& environment = task.environmentId;
    const auto pending = std::exchange(task.pendingDestination, std::nullopt);
    const auto showSettings = [&](std::string_view text) {
        const auto wait = waitStorage.currentWait(task.id);
        if (!wait || wait->reason() != WaitReason::settings || wait->text != text)
            waitStorage.wait(task, SimpleWaitReason::settings, std::string(text));
    };
    if (const auto* problem = std::get_if<std::string>(&catalog)) {
        showSettings(*problem);
        return;
    }
    const auto& projects = std::get<std::vector<T3Project>>(catalog);
    const auto remembered = rememberedT3Project(bot, environment);
    if (!remembered.empty() && std::ranges::find(projects, remembered, &T3Project::id) == projects.end())
        database.exec("DELETE FROM bot_t3_projects WHERE bot_id=? AND environment_id=? AND project_id=?", {bot, environment, remembered});
    if (projects.empty()) {
        showSettings("Add or unarchive a project in this T3 environment.");
        return;
    }
    const auto peer = peerStorage.peerExchange(task.id);
    const auto usable = [&](const T3Project& project) {
        return !peer || peer->recipient != task.id || peer->kind != "work" ||
               !peerStorage.peerWorkRouteWouldCycle(peer->sender, environment, project.id);
    };
    const auto preferred = [&](std::string_view id) {
        const auto match = std::ranges::find(projects, id, &T3Project::id);
        return match != projects.end() && usable(*match) ? match : projects.end();
    };
    auto selected = pending ? preferred(pending->route.projectId) : projects.end();
    if (selected == projects.end()) selected = preferred(remembered);
    if (selected == projects.end()) {
        selected = std::ranges::find_if(projects, usable);
        if (selected == projects.end()) {
            showSettings("Each project in this environment would make project queues wait for one another. "
                         "This task can continue after the related work finishes or after moving to another environment.");
            return;
        }
        if (std::find_if(std::next(selected), projects.end(), usable) != projects.end()) selected = projects.end();
    }
    waitStorage.clearWait(task, "connection_restored");
    task.state = TaskState::queued;
    if (selected != projects.end()) assignProject(task, selected->id);
}
}
