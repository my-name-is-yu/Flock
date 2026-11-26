#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/policy.hpp"
#include "channel_storage.hpp"
#include "runtime_lifecycle.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
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
#include "storage.hpp"
#include "wait_storage.hpp"
#include "policy_approvals.hpp"
#include "task_routing.hpp"
#include "flock/workspace_tools.hpp"
#include "outbox_storage.hpp"

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
    ChannelStorage& channelStorage;
    ConversationStorage& conversationTurns;
    GitStorage& gitStorage;
    WorkflowStorage& workflowStorage;
    Policy policy;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    runtime::Maintenance maintenance;
    runtime::SessionPrompts prompts;
    runtime::TaskUpdates updates;
    runtime::TaskQuestions questions;
    runtime::SessionHistory history;
    runtime::RunReports reports;
    runtime::PeerExchanges peerExchanges;
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

    Impl(Store::Impl& storage, Policy permissions, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : channelStorage(storage.channelStorage), conversationTurns(storage.conversationTurns), gitStorage(storage.gitStorage),
          workflowStorage(storage.workflowStorage), policy(permissions), model(m), t3(adapter), soul(std::move(loader)),
          options(withDefaultBotTools(std::move(opts))),
          maintenance(storage.database, storage.automationStorage, storage.workflowStorage, storage.policyApprovals, options.clock,
                      options.automationError, options.storageContention),
          prompts(storage.database, storage.secretStorage, storage.taskEvents, storage.workflowStorage),
          updates(storage.database, storage.journalStorage, storage.taskEvents, storage.taskRecords, storage.waitStorage),
          questions(storage.database, storage.waitStorage), history(storage.database, t3, policy, updates, options.sessionRead),
          reports(storage.database, storage.journalStorage, storage.taskEvents, storage.workflowStorage, t3, policy, updates, history,
                  options.clock),
          peerExchanges(storage.database, storage.botLifecycle, storage.conversationTurns, storage.journalStorage, storage.peerStorage,
                        storage.taskEvents, storage.taskOperations, storage.taskRecords, storage.taskRouting, storage.waitStorage, policy,
                        updates, reports, history, options.peers),
          workers(storage.conversationTurns, storage.sessionMessageStorage, storage.sessionParticipationStorage, channelStorage, t3,
                  maintenance),
          runs(storage.database, storage.botSettingsStorage, storage.journalStorage, storage.peerStorage, storage.secretStorage,
               storage.taskEvents, storage.taskRecords, storage.waitStorage, storage.outboxStorage, storage.workflowStorage, t3, updates,
               questions, reports, peerExchanges, options.clock, workers.stopToken()),
          dispatch(storage.database, storage.botLifecycle, storage.botSettingsStorage, storage.conversationTurns, storage.journalStorage,
                   storage.path, storage.policyApprovals, storage.sessionParticipationStorage, storage.sessionReadStorage,
                   storage.taskEvents, storage.taskRecords, storage.taskRouting, storage.taskRunReports, storage.waitStorage,
                   storage.outboxStorage, storage.memoryStorage, t3, policy, updates, questions, reports, runs, prompts, options.clock,
                   options.intake, options.requireT3Connection, workers.stopToken()),
          decisions(storage.database, storage.conversationTurns, storage.gitStorage, storage.journalStorage, storage.peerStorage,
                    storage.secretStorage, storage.taskEvents, storage.taskRecords, storage.waitStorage, updates, reports, history,
                    peerExchanges, dispatch, options.sessionRead, options.gitHost.get()),
          taskTurn(storage.database, storage.gitStorage, storage.modelCalls, storage.peerStorage, storage.secretStorage, storage.taskEvents,
                   storage.taskRecords, storage.taskRouting, storage.waitStorage, storage.memoryStorage, model, soul, options.clock,
                   options.gitHost.get(), options.sessionRead, workers.stopToken(), updates, reports, history, peerExchanges, runs, prompts,
                   decisions),
          taskScheduler(storage.database, storage.taskEvents, storage.taskRouting, t3, updates, dispatch, options.requireT3Connection,
                        workers.stopToken()),
          lock(storage.database, storage.path, storage.policyApprovals, options.clock) {}

    ~Impl() { workers.drain(taskTurn); }

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
        if (task.pendingStageReport)
            updates.change(task, [&](TaskData& current) { return workflowStorage.settleStageReport(current); });
        else if (runs.shouldStop(task))
            dispatch.stop(task);
        else if (task.pendingRunReport)
            reports.recoverRunReport(task);
        else if (state == TaskState::queued)
            taskScheduler.activate(std::move(task));
        else if (state == TaskState::deciding) {
            if (!decisions.resumeGitApproval(task) && !decisions.continueStage(task)) taskTurn.decide(task);
        } else {
            if (task.activeRun && (state == TaskState::running || state == TaskState::waiting) && runs.ingest(task, runs.observe(task)))
                return;
            dispatch.step(task);
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul) : Runtime(store, model, t3, std::move(soul), {}) {}

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(*store.p_, Policy{store}, model, t3, std::move(soul), std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::requestStop() {
    p_->workers.requestStop();
}
void Runtime::runConversations() {
    if (p_->workers.stopToken().stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->gitStorage.deliverGitWebhooks();
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
    p_->taskTurn.persistTaskInferences();
    p_->maintenance.fireDueAutomations();
    p_->gitStorage.deliverGitWebhooks();
    p_->workers.wakeSessions();
    for (const auto& [bot, id] : p_->conversationTurns.pendingConversations(runtime::Workers::batchLimit, p_->options.clock()))
        p_->runTurn(id);
    p_->maintenance.advanceWorkflows();
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
    p_->maintenance.deferContendedWork("Git webhooks", [&] { p_->gitStorage.deliverGitWebhooks(); });
    p_->workers.startChannels();
    p_->workers.stepSessionWake();
    p_->workers.startConversations(p_->options.clock, [impl = p_.get()](Id id) { impl->runTurn(id); });
    p_->maintenance.advanceWorkflowsIfChanged();
    p_->maintenance.deferContendedWork("task scheduling", [&] {
        p_->taskTurn.persistTaskInferences();
        p_->workers.startTasks(p_->taskScheduler, [impl = p_.get()](const auto& work) { impl->step(work); });
    });
}
} // namespace flockv0
