#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
std::string Store::Impl::rememberedT3Project(std::string_view bot, std::string_view environment) const {
    auto row = query("SELECT project_id FROM bot_t3_projects WHERE bot_id=? AND environment_id=?", {bot, environment});
    return row.next() ? row.string(0) : std::string{};
}

void Store::Impl::rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project) {
    exec("INSERT INTO bot_t3_projects VALUES(?,?,?) ON CONFLICT(bot_id,environment_id) "
         "DO UPDATE SET project_id=excluded.project_id",
         {bot, environment, project});
}

bool Store::Impl::projectQuestionOpen(std::string_view bot, std::string_view environment) const {
    return query("SELECT 1 FROM tasks t JOIN waits w ON w.task_id=t.id "
                 "WHERE t.bot_id=? AND t.environment_id=? AND t.project_id='' AND w.resolution IS NULL "
                 "AND json_type(w.data,'$.projectChoices')='object' LIMIT 1",
                 {bot, environment})
        .next();
}

ProjectCatalog Store::Impl::readProjectCatalog(T3& t3, std::string_view bot, std::string_view environment, bool requireConnection) {
    if (requireConnection && !t3EnvironmentConnected(environment)) return "Connect this T3 environment in Settings, then try again.";
    Policy policy(*this);
    if (!policy.allows(bot, Action::environment_inspect, environment) || policy.forbidden(bot, Action::environment_launch, environment))
        return "Flock cannot use this T3 environment under the current permissions.";
    try {
        auto projects = t3.projects(environment);
        const auto scrub = redactor(bot);
        std::erase_if(projects, [&](const T3Project& project) { return scrub.text(project.id) != project.id; });
        return projects;
    } catch (const std::exception&) {
        return "Flock could not list projects. Check the T3 connection and try again.";
    }
}

namespace {
ProjectQuestionWait projectQuestion(const std::vector<T3Project>& projects, const SecretRedactor& scrub) {
    std::map<std::string, std::size_t> titles;
    std::vector<std::string> names;
    for (const auto& project : projects) {
        names.push_back(bounded(scrub.text(project.title), 140));
        ++titles[names.back()];
    }
    ProjectQuestionWait question;
    for (std::size_t index = 0; index < projects.size(); ++index) {
        const auto& project = projects[index];
        auto label = names[index];
        if (titles.at(label) > 1 && !project.directory.empty()) label += " (" + bounded(scrub.text(project.directory), 40) + ")";
        const auto base = label;
        for (int suffix = 2; question.projects.contains(label); ++suffix) label = base + " #" + std::to_string(suffix);
        question.choices.push_back(label);
        question.projects.emplace(label, project.id);
    }
    return question;
}
}

bool Store::Impl::applyProjectCatalog(TaskData& task, ProjectCatalog catalog) {
    const auto& bot = task.botId;
    const auto& environment = task.environmentId;
    const auto pending = std::exchange(task.pendingDestination, std::nullopt);
    if (const auto* problem = std::get_if<std::string>(&catalog)) {
        wait(task, SimpleWaitReason::settings, *problem);
        return true;
    }
    auto& projects = std::get<std::vector<T3Project>>(catalog);
    const auto remembered = rememberedT3Project(bot, environment);
    if (!remembered.empty() && std::ranges::find(projects, remembered, &T3Project::id) == projects.end())
        exec("DELETE FROM bot_t3_projects WHERE bot_id=? AND environment_id=? AND project_id=?", {bot, environment, remembered});
    if (projects.empty()) {
        wait(task, SimpleWaitReason::settings, "Add or unarchive a project in this T3 environment, then try again.");
        return true;
    }
    const auto id = task.id;
    const auto peer = peerExchange(id);
    if (peer && peer->recipient == id && peer->kind == "work")
        std::erase_if(projects, [&](const T3Project& project) { return peerWorkRouteWouldCycle(peer->sender, environment, project.id); });
    if (projects.empty()) {
        wait(task, SimpleWaitReason::settings,
             "Each project in this environment would make project queues wait for one another. "
             "Move this task to another environment or retry after related work finishes.");
        return true;
    }
    auto selected = pending ? std::ranges::find(projects, pending->route.projectId, &T3Project::id) : projects.end();
    if (selected == projects.end()) selected = std::ranges::find(projects, remembered, &T3Project::id);
    if (selected == projects.end() && projects.size() == 1) selected = projects.begin();
    if (selected != projects.end()) {
        task.projectId = selected->id;
        if (task.rest.contains("runTargets"))
            for (auto& target : task.rest["runTargets"])
                if (field(target, "environmentId") == environment && field(target, "projectId").empty()) target["projectId"] = selected->id;
        rememberT3Project(bot, environment, selected->id);
        return true;
    }
    if (projectQuestionOpen(bot, environment)) return false;
    wait(task, projectQuestion(projects, redactor(bot)), "Which T3 project should this Bot use in this environment?");
    return true;
}
}
