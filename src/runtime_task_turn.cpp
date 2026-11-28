#include "runtime_task_turn.hpp"

#include "bot_setup_storage.hpp"
#include "database.hpp"
#include "memory_storage.hpp"
#include "flock/bot_notes_error.hpp"
#include "flock/model.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_task_decisions.hpp"
#include "runtime_task_scheduler.hpp"
#include "runtime_task_updates.hpp"
#include "task_events.hpp"
#include "task_routing.hpp"
#include "wait_storage.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::TaskTurn::TaskTurn(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, WaitStorage& waitStorage,
                            TaskUpdates& updates, RunObserver& runs, TaskDecisions& decisions, TaskScheduler& scheduler,
                            BotSetupStorage& botSetup, MemoryStorage& memoryStorage, const SoulLoader& soul, const IntakeResolver& intake,
                            const std::function<Seconds()>& clock, std::stop_token stop)
    : database(database), taskEvents(taskEvents), taskRouting(taskRouting), waitStorage(waitStorage), updates(updates), runs(runs),
      decisions(decisions), scheduler(scheduler), botSetup(botSetup), memoryStorage(memoryStorage), soul(soul), intake(intake),
      clock(clock), stop(std::move(stop)) {}

bool runtime::TaskTurn::resumeNotes(const TaskData& task, const Wait& wait) {
    if (wait.reason() != WaitReason::bot_notes_unavailable) return false;
    try {
        if (task.actionId.empty()) launchDefaults(task);
        memoryStorage.loadNotes(task.botId, soul, clock());
    } catch (const BotNotesUnavailable&) {
        return true;
    } catch (const std::filesystem::filesystem_error&) {
        return true;
    }
    updates.change(task, [&](TaskData& current) {
        const auto openWait = waitStorage.currentWait(current.id);
        if (current.state != TaskState::waiting || current.replanning || !openWait || openWait->id != wait.id) return false;
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

void runtime::TaskTurn::decide(const TaskData& task) {
    if (task.activeRun && !runs.questionStillOpen(task)) return;
    if (stop.stop_requested()) return;
    Json event;
    {
        std::lock_guard lock(database.mutex);
        event = taskEvents.currentEvent(task);
    }
    const bool idle = task.actionId.empty() && !task.activeRun;
    const auto kind = field(event, "kind");
    const bool eventReady = !event.is_null() && event.at("consumed") != true && event.at("retryAt").get<Seconds>() <= clock();
    const bool launch = idle && eventReady && kind == "intake";
    const bool hasRun = RunReports::hasSettledRun(task);
    const bool fresh = idle && task.freshContextRequested && hasRun && !task.pendingInstructions.empty() &&
                       (event.is_null() || event.at("consumed") == true || (kind == "result" && !event.contains("userAnswer")));
    const bool answered = event.contains("userAnswer");
    const bool instruct = idle && eventReady && (kind != "policy_outcome" || answered) && (!task.pendingInstructions.empty() || answered);
    const bool newSession = launch || ((fresh || instruct) && (!hasRun || task.newSessionRequired));
    const bool useSavedSession = task.selectedSession && !task.newSessionRequired;
    if (newSession && task.projectId.empty() && task.runTargets().empty() && !useSavedSession) return scheduler.activate(task);

    std::optional<Json> defaults;
    if (newSession && !useSavedSession) {
        try {
            defaults = launchDefaults(task);
        } catch (const std::filesystem::filesystem_error&) {
            updates.change(task, [&](TaskData& current) {
                if (current.state != TaskState::deciding || current.replanning || !current.actionId.empty()) return false;
                waitStorage.wait(current, SimpleWaitReason::bot_notes_unavailable,
                                 "Restore access to this Bot’s profile in Flock settings.");
                return true;
            });
            return;
        }
    }
    if (fresh && decisions.continueFreshSession(task, defaults ? &*defaults : nullptr)) return;
    if (!eventReady) return;
    if (launch) return decisions.launch(task, event, defaults ? &*defaults : nullptr);
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
