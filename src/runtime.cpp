#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/policy.hpp"
#include "channel_storage.hpp"
#include "runtime_lifecycle.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_prompts.hpp"
#include "runtime_task_decisions.hpp"
#include "runtime_task_questions.hpp"
#include "runtime_task_scheduler.hpp"
#include "runtime_task_turn.hpp"
#include "flock/t3.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "store_access.hpp"
#include "wait_storage.hpp"
#include "policy_approvals.hpp"
#include "task_routing.hpp"
#include "task_events.hpp"
#include "flock/workspace_tools.hpp"
#include "outbox_storage.hpp"
#include "database.hpp"

#include <memory>
#include <utility>

namespace flockv0 {
namespace {
RuntimeOptions withDefaultBotTools(RuntimeOptions options) {
    if (!options.botTools) options.botTools = std::make_shared<BotTools>();
    return options;
}
}

struct Runtime::Impl {
    Database& database;
    WaitStorage& waitStorage;
    ChannelStorage& channelStorage;
    ConversationStorage& conversationTurns;
    GitStorage& gitStorage;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    Policy policy;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    runtime::Maintenance maintenance;
    runtime::SessionPrompts prompts;
    runtime::TaskUpdates updates;
    runtime::TaskQuestions questions;
    runtime::RunReports reports;
    runtime::Workers workers;
    runtime::RunObserver runs;
    runtime::OutboxDispatch dispatch;
    runtime::TaskDecisions decisions;
    runtime::TaskTurn taskTurn;
    runtime::TaskScheduler taskScheduler;
    runtime::DatabaseLock lock;

    void runTurn(Id id) {
        conversationTurns.runTurn(id, model, t3, soul, options.clock, *options.botTools, options.gitHost.get(), options.applyBotChange,
                                  options.reconcileBotCreation, options.intake, options.peers, workers.stopToken());
    }

    void wakeExternalEvents() {
        gitStorage.deliverGitWebhooks();
        taskEvents.wakeExternalChecks(taskRecords, options.clock());
    }

    Impl(StoreAccess::Impl& storage, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : database(storage.database), waitStorage(storage.waitStorage), channelStorage(storage.channelStorage),
          conversationTurns(storage.conversationTurns), gitStorage(storage.gitStorage), taskEvents(storage.taskEvents),
          taskRecords(storage.taskRecords), policy(storage.permissions), model(m), t3(adapter), soul(std::move(loader)),
          options(withDefaultBotTools(std::move(opts))), maintenance(storage.database, storage.automationStorage, storage.policyApprovals,
                                                                     options.clock, options.automationError, options.storageContention),
          prompts(storage.database, storage.secretStorage, storage.memoryStorage, storage.taskEvents, soul),
          updates(storage.database, storage.journalStorage, storage.taskEvents, storage.taskRecords),
          questions(storage.database, storage.waitStorage),
          reports(storage.database, storage.journalStorage, storage.taskEvents, t3, policy, updates, options.clock),
          workers(storage.conversationTurns, storage.sessionMessageStorage, storage.sessionParticipationStorage, channelStorage, t3,
                  maintenance),
          runs(storage.database, storage.journalStorage, storage.secretStorage, storage.taskEvents, storage.taskRecords,
               storage.waitStorage, storage.outboxStorage, t3, updates, questions, reports, options.clock, workers.stopToken()),
          dispatch(storage.database, storage.botLifecycle, storage.conversationMessages, storage.journalStorage, storage.path,
                   storage.policyApprovals, storage.sessionParticipations, storage.sessionReadStorage, storage.taskEvents,
                   storage.taskRecords, storage.taskRouting, storage.taskRunReports, storage.waitStorage, storage.outboxStorage, t3, policy,
                   updates, questions, reports, runs, prompts, options.clock, options.requireT3Connection, options.intake,
                   workers.stopToken()),
          decisions(storage.conversationTurns, storage.taskEvents, storage.taskRecords, storage.waitStorage, updates, dispatch),
          taskTurn(storage.database, storage.taskEvents, storage.taskRouting, storage.waitStorage, updates, runs, decisions,
                   storage.botSetup, storage.memoryStorage, soul, options.intake, t3, options.clock, workers.stopToken(),
                   options.requireT3Connection),
          taskScheduler(storage.database, storage.taskEvents, storage.taskRouting, t3, updates, options.requireT3Connection, options.clock,
                        workers.stopToken()),
          lock(storage.database, storage.path, storage.policyApprovals, options.clock) {}

    ~Impl() { workers.drain(); }

    void step(const runtime::TaskScheduler::Work& work) {
        if (workers.stopToken().stop_requested()) return;
        const auto id = work.taskId;
        auto task = updates.snapshot(id);
        const auto state = task.state;
        if (work.destinationAction) {
            auto& run = destinationRun(task, *work.destinationAction);
            if (run.outstanding()) dispatch.stepDestination(task, run);
            return;
        }
        if (!task.runResults().empty()) return;
        if (runs.shouldStop(task))
            dispatch.stop(task);
        else if (task.pendingRunReport)
            reports.recoverRunReport(task);
        else if (state == TaskState::queued)
            taskScheduler.activate(std::move(task));
        else if (state == TaskState::deciding) {
            if (dispatch.step(task, nullptr)) task = updates.snapshot(id);
            if (task.state != TaskState::deciding) return;
            const auto defaults = task.freshContextRequested && task.newSessionRequired ? taskTurn.launchDefaults(task) : std::nullopt;
            if (!decisions.continueFreshSession(task, defaults ? &*defaults : nullptr)) taskTurn.decide(task);
        } else {
            if (task.activeRun && (state == TaskState::running || state == TaskState::waiting || state == TaskState::sending) &&
                runs.ingest(task, runs.observe(task)))
                return;
            std::optional<Wait> wait;
            if (state == TaskState::waiting) {
                std::lock_guard guard(database.mutex);
                wait = waitStorage.currentWait(id);
            }
            if (wait && taskTurn.resumeNotes(task, *wait)) return;
            if (wait && wait->reason() == WaitReason::settings && task.projectId.empty()) {
                taskScheduler.activate(std::move(task));
                return;
            }
            dispatch.step(task, wait ? &*wait : nullptr);
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul) : Runtime(store, model, t3, std::move(soul), {}) {}

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(StoreAccess::get(store), model, t3, std::move(soul), std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::requestStop() {
    p_->workers.requestStop();
}
void Runtime::runConversations() {
    if (p_->workers.stopToken().stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->wakeExternalEvents();
    p_->workers.wakeSessions();
    p_->channelStorage.deliverChannels(p_->workers.stopToken());
    constexpr auto messagesPerTick = 20;
    for (int i = 0; i < messagesPerTick && !p_->workers.stopToken().stop_requested(); ++i) {
        const auto pending = p_->conversationTurns.pendingConversations(1, p_->options.clock());
        if (pending.empty()) break;
        p_->runTurn(pending.front().second);
    }
    p_->channelStorage.deliverChannels(p_->workers.stopToken());
}
void Runtime::tick() {
    if (p_->workers.stopToken().stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->maintenance.fireDueAutomations();
    p_->wakeExternalEvents();
    p_->workers.wakeSessions();
    for (const auto& [bot, id] : p_->conversationTurns.pendingConversations(runtime::Workers::batchLimit, p_->options.clock()))
        p_->runTurn(id);
    for (const auto& work :
         p_->taskScheduler.nextBatch(runtime::Workers::batchLimit, [this](const auto& job) { return p_->workers.isActive(job); }))
        p_->step(work);
    p_->channelStorage.deliverChannels(p_->workers.stopToken());
}
void Runtime::tickConcurrent() {
    if (p_->workers.stopToken().stop_requested()) return;
    p_->maintenance.settleApprovalsIfChanged();
    p_->workers.collectCompleted();
    p_->maintenance.fireDueAutomationsIfChanged();
    p_->maintenance.deferContendedWork("external task events", [&] { p_->wakeExternalEvents(); });
    p_->workers.startChannels();
    p_->workers.stepSessionWake();
    p_->workers.startConversations(p_->options.clock, [impl = p_.get()](Id id) { impl->runTurn(id); });
    p_->maintenance.deferContendedWork(
        "task scheduling", [&] { p_->workers.startTasks(p_->taskScheduler, [impl = p_.get()](const auto& work) { impl->step(work); }); });
}
} // namespace flockv0
