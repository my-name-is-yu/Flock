#include "storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_peer.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_task_scheduler.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_task_decisions.hpp"
#include "runtime_task_turn.hpp"
#include "runtime_session_prompts.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_task_questions.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_context_storage.hpp"
#include "task_interaction_storage.hpp"
#include "task_storage.hpp"
#include "commit_check.hpp"
#include "conversation_storage.hpp"
#include "flock/bot_profile.hpp"
#include "flock/participation.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace flockv0 {
namespace {}

struct Runtime::Impl {
    Store::Impl& db;
    Policy policy;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    Maintenance maintenance;
    TaskScheduler taskScheduler;
    SessionPrompts prompts;
    TaskUpdates updates;
    TaskQuestions questions;
    SessionHistory history;
    RunReports reports;
    PeerExchanges peerExchanges;
    std::future<void> channelWorker, sessionWakeWorker;
    std::stop_source shutdown;
    RunObserver runs;
    OutboxDispatch dispatch;
    TaskDecisions decisions;
    TaskTurn taskTurn;
    int lockFd = -1;
    std::map<TaskScheduler::Work, std::future<void>> workers;
    std::map<std::string, std::future<void>> conversationWorkers;

    void runTurn(Id id) {
        db.runTurn(id, model, t3, soul, options.clock, *options.botTools, options.gitHost.get(), options.applyBotChange,
                   options.reconcileBotCreation, options.intake, options.peers, shutdown.get_token());
    }

    Impl(Store& store, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : db(*store.p_), policy(store.policy()), model(m), t3(adapter), soul(std::move(loader)), options(std::move(opts)),
          maintenance(db, options.clock, options.automationError, options.storageContention), taskScheduler(db), prompts(db), updates(db),
          questions(db), history(db, t3, policy, updates, options.sessionRead), reports(db, t3, policy, updates, history, options.clock),
          peerExchanges(db, policy, updates, reports, history, options.peers),
          runs(db, t3, updates, questions, reports, peerExchanges, options.clock, shutdown.get_token()),
          dispatch(db, t3, policy, updates, questions, reports, runs, prompts, options.clock, options.intake, options.requireT3Connection,
                   shutdown.get_token()),
          decisions(db, updates, reports, history, peerExchanges, dispatch, options.sessionRead, options.gitHost.get()),
          taskTurn(db, model, soul, options.clock, options.gitHost.get(), options.sessionRead, shutdown.get_token(), updates, reports,
                   history, peerExchanges, runs, prompts, decisions) {
        if (!options.botTools) options.botTools = std::make_shared<BotTools>();
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = options.clock;
        }
    }
    ~Impl() {
        shutdown.request_stop();
        conversationWorkers.clear();
        workers.clear();
        if (channelWorker.valid()) channelWorker.wait();
        if (sessionWakeWorker.valid()) sessionWakeWorker.wait();
        try {
            taskTurn.persistTaskInferences();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Flock Task inference shutdown: %s\n", error.what());
        }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = unixTime;
        }
        close(lockFd);
    }

    static constexpr unsigned batchLimit = 4;

    bool selectProject(TaskData& task) {
        const auto bot = task.botId;
        const auto environment = task.environmentId;
        {
            std::lock_guard lock(db.mutex);
            if (db.projectQuestionOpen(bot, environment)) return false;
        }
        auto catalog = db.readProjectCatalog(t3, bot, environment, options.requireT3Connection);
        return updates.change(
                   task,
                   [&](TaskData& current) { return !shutdown.stop_requested() && db.applyProjectCatalog(current, std::move(catalog)); },
                   &task) &&
               !task.projectId.empty();
    }

    void activate(TaskData task) {
        if (task.projectId.empty() && !selectProject(task)) return;
        updates.change(task, [&](TaskData& current) {
            auto turn = db.query("SELECT 1 FROM task_project_slots WHERE id=? AND blocker_id IS NULL", {current.id});
            if (!turn.next()) return false;
            if (current.workflow && !current.lastRun) {
                dispatch.queueAction(current, actionId(), CommandTool::delegate, "", Json::object(), Json::object(), false);
                return true;
            }
            if (current.eventId == 0) db.event(current, "intake", {{"kind", "intake"}});
            current.state = TaskState::deciding;
            return true;
        });
    }

    void step(const TaskScheduler::Work& work) {
        if (shutdown.stop_requested()) return;
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
            updates.change(task, [&](TaskData& current) { return db.settleStageReport(current); });
        else if (runs.shouldStop(task))
            dispatch.stop(task);
        else if (task.pendingRunReport)
            reports.recoverRunReport(task);
        else if (state == TaskState::queued)
            activate(task);
        else if (state == TaskState::deciding) {
            if (!decisions.resumeGitApproval(task) && !decisions.continueStage(task)) taskTurn.decide(task);
        } else {
            if (task.activeRun && (state == TaskState::running || state == TaskState::waiting) && runs.ingest(task, runs.observe(task)))
                return;
            dispatch.step(task);
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(store, model, t3, std::move(soul), std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::requestStop() {
    p_->shutdown.request_stop();
}
void Runtime::runConversations() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3, p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3, p_->shutdown.get_token());
    p_->db.deliverChannels(p_->shutdown.get_token());
    constexpr auto messagesPerTick = 20;
    for (int i = 0; i < messagesPerTick && !p_->shutdown.stop_requested(); ++i) {
        const auto pending = p_->db.pendingConversations(1, p_->options.clock());
        if (pending.empty()) break;
        p_->runTurn(pending.front().second);
    }
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tick() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->taskTurn.persistTaskInferences();
    p_->maintenance.fireDueAutomations();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3, p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3, p_->shutdown.get_token());
    for (const auto& [bot, id] : p_->db.pendingConversations(Impl::batchLimit, p_->options.clock())) p_->runTurn(id);
    p_->maintenance.advanceWorkflows();
    for (const auto& work : p_->taskScheduler.nextBatch(Impl::batchLimit, [this](const auto& job) { return p_->workers.contains(job); }))
        p_->step(work);
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tickConcurrent() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovalsIfChanged();
    const auto collect = [this](auto& workers, std::string_view operation) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = std::move(it->second);
                it = workers.erase(it);
                p_->maintenance.deferContendedWork(operation, [&] { result.get(); });
            } else
                ++it;
        }
    };
    collect(p_->conversationWorkers, "conversation worker");
    collect(p_->workers, "task worker");
    if (p_->channelWorker.valid() && p_->channelWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->maintenance.deferContendedWork("channel worker", [&] { p_->channelWorker.get(); });
    p_->maintenance.fireDueAutomationsIfChanged();
    p_->maintenance.deferContendedWork("Git webhooks", [&] { p_->db.deliverGitWebhooks(); });
    if (!p_->channelWorker.valid() && p_->db.channelDeliveryDue())
        p_->channelWorker = std::async(std::launch::async, [db = &p_->db, stop = p_->shutdown.get_token()] { db->deliverChannels(stop); });
    if (p_->sessionWakeWorker.valid() && p_->sessionWakeWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->maintenance.deferContendedWork("Session wake worker", [&] { p_->sessionWakeWorker.get(); });
    if (!p_->sessionWakeWorker.valid())
        p_->sessionWakeWorker = std::async(std::launch::async, [impl = p_.get()] {
            impl->db.settleSessionQueueCancellations(impl->t3, impl->shutdown.get_token());
            impl->db.wakeSessionMessages(impl->t3, impl->shutdown.get_token());
        });
    constexpr auto maxConversationWorkers = Impl::batchLimit;
    if (p_->conversationWorkers.size() < maxConversationWorkers) {
        for (const auto& [bot, id] : p_->db.pendingConversations(maxConversationWorkers, p_->options.clock())) {
            if (p_->conversationWorkers.size() >= maxConversationWorkers) break;
            if (!p_->conversationWorkers.contains(bot)) {
                p_->conversationWorkers.emplace(bot, std::async(std::launch::async, [impl = p_.get(), id] { impl->runTurn(id); }));
            }
        }
    }
    p_->maintenance.advanceWorkflowsIfChanged();
    p_->maintenance.deferContendedWork("task scheduling", [&] {
        p_->taskTurn.persistTaskInferences();
        for (const auto& work : p_->taskScheduler.nextBatch(Impl::batchLimit - p_->workers.size(),
                                                            [this](const auto& job) { return p_->workers.contains(job); }))
            p_->workers.emplace(work, std::async(std::launch::async, [impl = p_.get(), work] { impl->step(work); }));
    });
}
} // namespace flockv0
