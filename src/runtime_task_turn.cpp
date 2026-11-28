#include "runtime_task_turn.hpp"

#include "bot_setup_storage.hpp"
#include "database.hpp"
#include "memory_storage.hpp"
#include "flock/bot_notes_error.hpp"
#include "flock/model.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_task_decisions.hpp"
#include "runtime_task_updates.hpp"
#include "task_events.hpp"
#include "task_routing.hpp"
#include "wait_storage.hpp"
#include "flock/task.hpp"
#include "flock/t3.hpp"
#include "flock/wait.hpp"

#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::TaskTurn::TaskTurn(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, WaitStorage& waitStorage,
                            TaskUpdates& updates, RunObserver& runs, TaskDecisions& decisions, BotSetupStorage& botSetup,
                            MemoryStorage& memoryStorage, const SoulLoader& soul, const IntakeResolver& intake, T3& t3,
                            const std::function<Seconds()>& clock, std::stop_token stop, bool requireT3Connection)
    : database(database), taskEvents(taskEvents), taskRouting(taskRouting), waitStorage(waitStorage), updates(updates), runs(runs),
      decisions(decisions), botSetup(botSetup), memoryStorage(memoryStorage), soul(soul), intake(intake), t3(t3), clock(clock),
      stop(std::move(stop)), requireT3Connection(requireT3Connection) {}

bool runtime::TaskTurn::resumeNotes(const TaskData& task, const Wait& wait) {
    if (wait.reason() != WaitReason::bot_notes_unavailable) return false;
    try {
        memoryStorage.loadNotes(task.botId, soul, clock());
    } catch (const BotNotesUnavailable&) {
        return true;
    }
    updates.change(task, [&](TaskData& current) {
        const bool requeue = taskRouting.isUndispatchedNotesWait(current);
        waitStorage.clearWait(current, "bot_setup_restored");
        current.state = requeue ? TaskState::queued : !current.actionId.empty() ? TaskState::sending : TaskState::deciding;
        return true;
    });
    return true;
}

std::optional<Json> runtime::TaskTurn::launchDefaults(const TaskData& task) const {
    if (task.selectedSession && !task.newSessionRequired) return std::nullopt;
    const auto live = botSetup.liveIntake(intake, task.botId, task.intent);
    if (!live) return std::nullopt;
    return live->config.value("session", Json::object());
}

// A task without a project, for example after a move, takes its reserved, remembered or only project.
bool runtime::TaskTurn::resolveProject(const TaskData& task, const Json& event) {
    const auto& pending = task.pendingDestination;
    auto project = pending && pending->route.environmentId == task.environmentId ? pending->route.projectId : std::string{};
    if (project.empty()) {
        std::lock_guard lock(database.mutex);
        project = taskRouting.rememberedT3Project(task.botId, task.environmentId);
    }
    if (project.empty()) {
        auto catalog = taskRouting.readProjectCatalog(t3, task.botId, {task.environmentId, {}, task.gatewayEnvironmentId.value_or("")},
                                                      requireT3Connection);
        if (const auto* problem = std::get_if<std::string>(&catalog)) {
            updates.change(task, [&](TaskData& current) {
                waitStorage.wait(current, SimpleWaitReason::settings, *problem);
                return true;
            });
            return true;
        }
        const auto& projects = std::get<std::vector<T3Project>>(catalog);
        if (projects.empty()) {
            updates.change(task, [&](TaskData& current) {
                waitStorage.wait(current, SimpleWaitReason::settings, "Add or unarchive a project in this T3 environment.");
                return true;
            });
            return true;
        }
        if (projects.size() == 1) project = projects.front().id;
    }
    if (project.empty()) {
        decisions.handle(task, event, nullptr);
        return true;
    }
    updates.change(task, [&](TaskData& current) {
        taskRouting.assignProject(current, project);
        current.state = TaskState::queued;
        return true;
    });
    return true;
}

void runtime::TaskTurn::decide(const TaskData& task) {
    if (task.activeRun && !runs.questionStillOpen(task)) return;
    if (stop.stop_requested()) return;
    Json event;
    {
        std::lock_guard lock(database.mutex);
        event = taskEvents.currentEvent(task);
        if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > clock()) return;
    }
    if (task.projectId.empty() && task.runTargets().empty() && !task.selectedSession && task.actionId.empty() && !task.activeRun &&
        resolveProject(task, event))
        return;
    const auto defaults = launchDefaults(task);
    if (event["kind"] == "intake" && task.actionId.empty() && !task.activeRun)
        return decisions.launch(task, event, defaults ? &*defaults : nullptr);
    if (event.contains("userAnswer")) {
        try {
            if (decisions.answerSessionQuestion(task, event)) return;
        } catch (const Conflict&) {
            // A rejected answer would be rejected again on every retry, so it falls through to the closure rule.
        }
    }
    decisions.handle(task, event, defaults ? &*defaults : nullptr);
}
}
