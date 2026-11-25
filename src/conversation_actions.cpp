#include "storage.hpp"
#include "journal.hpp"
#include "journal_conversation.hpp"
#include "flock/limits.hpp"
#include "conversation_storage.hpp"
#include "commit_check.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/decision.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
using conversation_storage::identifier;
using conversation_storage::text;
using conversation_storage::ToolError;

namespace {
const DestinationRun& selectSavedDestination(const TaskData& task, const Json& args, bool allowOmitted) {
    if (task.runResults().empty()) throw ToolError("The destination is unavailable");
    const bool hasName = args.contains("destination");
    const bool hasEnvironment = args.contains("environmentId");
    const bool hasProject = args.contains("projectId");
    const bool hasGateway = args.contains("gatewayEnvironmentId");
    if (hasName && (hasEnvironment || hasProject || hasGateway))
        throw ToolError("Select a destination either by friendly name or by environmentId and projectId, not both");
    if (hasEnvironment != hasProject) throw ToolError("Select a destination by both environmentId and projectId");
    if (hasGateway && !hasEnvironment) throw ToolError("Select gatewayEnvironmentId only with environmentId and projectId");
    const auto name = text(args, "destination", 256, false);
    const auto environment = text(args, "environmentId", 256, false);
    const auto project = text(args, "projectId", 256, false);
    const auto gateway = text(args, "gatewayEnvironmentId", 256, false);
    if ((hasName && name.empty()) || (hasEnvironment && (environment.empty() || project.empty())))
        throw ToolError("Destination selectors must not be empty");
    if (hasGateway && gateway.empty()) throw ToolError("gatewayEnvironmentId must not be empty");

    if (!hasName && !hasEnvironment) {
        if (!allowOmitted) throw ToolError("Select a destination by friendly name or environmentId and projectId");
        if (task.runResults().size() != 1) throw ToolError("Choose a destination by friendly name or environmentId and projectId");
        return task.runResults().front();
    }
    const DestinationRun* selected = nullptr;
    for (const auto& run : task.runResults()) {
        const auto& savedGateway = run.ref.gatewayEnvironmentId.empty() ? run.environmentId : run.ref.gatewayEnvironmentId;
        const bool matches =
            hasName ? run.name == name
                    : run.environmentId == environment && run.assignment.projectId == project && (!hasGateway || savedGateway == gateway);
        if (!matches) continue;
        if (selected) throw ToolError("The destination selector is ambiguous");
        selected = &run;
    }
    if (!selected) throw ToolError("The destination is unavailable");
    return *selected;
}

struct SessionEffect {
    Id taskId, generation;
    AssignedRun run;
    Command command;

    Json checkpoint() const {
        Json effect = {{"kind", "interrupt"},
                       {"taskId", taskId},
                       {"environmentId", command.environmentId},
                       {"ownerBotId", command.ownerBotId},
                       {"projectId", command.projectId},
                       {"config", command.config},
                       {"run", assignedRunJson(run)},
                       {"actionId", command.actionId}};
        return effect;
    }

    Json dispatch(T3& t3) const {
        const auto outcome = t3.interrupt(command.environmentId, command.config, command.target);
        if (outcome == InterruptOutcome::Requested) return {{"ok", true}, {"effect", "interrupt_requested"}, {"taskId", taskId}};
        if (outcome == InterruptOutcome::AlreadyTerminal)
            return {{"ok", false}, {"taskId", taskId}, {"outcome", "already_terminal"}, {"error", "This work already ended"}};
        return {{"ok", false},
                {"taskId", taskId},
                {"outcome", "no_active_run"},
                {"error", "There is no work in progress to stop for this request"}};
    }
};
}

Store::Impl::CallOutcome Store::Impl::runSessionEffect(const conversation::Message& message, int step, const Decision& decision,
                                                       std::string_view key, bool offered, bool hasWebEvidence, T3& t3,
                                                       std::stop_token shutdown) {
    using enum CallOutcome;
    const auto id = message.id;
    const auto fail = [&](const ToolError& error) {
        auto failure = error.details;
        failure["ok"] = false;
        failure["error"] = error.what();
        saveTurnResults(id, step, Json::array({redactor(message.bot).record(std::move(failure))}));
    };
    const auto& args = decision.args;
    const auto admit = [&](Id taskId, Id expected, const AssignedRun* expectedRun) {
        const auto task = ownTask(taskId, message);
        const auto& run = task.activeRun;
        if (expected != task.generation || (expectedRun && run != *expectedRun))
            throw ToolError(
                "stale",
                {{"applied", 0}, {"taskId", taskId}, {"expected", {{"generation", expected}}}, {"current", taskState(task, message)}});
        if (terminal(task) || task.state == TaskState::cancelling)
            throw ToolError("The task is " + std::string(taskStateName(task.state)), {{"taskId", taskId}});
        if (task.replanning || task.runStop)
            throw ToolError("The task is stopping or replanning", {{"taskId", taskId}, {"current", taskState(task, message)}});
        if (task.hasDestinations())
            throw ToolError("Session actions are not available for tasks with destination Runs; use cancel_task to stop the whole task",
                            {{"taskId", taskId}});
        if (!run || run->ref.threadId.empty() || run->ref.runId.empty())
            throw ToolError("The task has no running Session. Use send_to_task to pass it on at the next safe point",
                            {{"taskId", taskId}, {"current", taskState(task, message)}});
        return task;
    };
    std::optional<SessionEffect> effect;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        auto saved = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? "
                           "AND result IS NULL AND effect IS NOT NULL",
                           {id, step});
        if (saved.next()) {
            saveTurnResults(id, step,
                            Json::array({Json{{"ok", false},
                                              {"taskId", saved.json(0).at("taskId")},
                                              {"delivery", "unknown"},
                                              {"error", "Flock restarted while sending this, so T3 may have received it. "
                                                        "Check the Session before sending it again"}}}));
        } else if (!offered) {
            saveTurnResults(id, step, Json::array({toolFailure("Session control was not offered for this turn")}));
        } else if (hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            saveTurnResults(id, step, Json::array({webEvidenceActionDenied()}));
        } else {
            try {
                if (!args.is_object()) throw ToolError("Tool arguments must be an object");
                const auto taskId = identifier(args, "taskId", true);
                const auto expected = identifier(args, "generation", true, 0);
                const auto task = admit(taskId, expected, nullptr);
                const auto& run = *task.activeRun;
                Command command;
                command.actionId = "flock-turn-" + std::to_string(id) + "-" + std::to_string(step);
                command.ownerBotId = message.bot;
                command.environmentId = run.environmentId;
                command.projectId = run.assignment.projectId;
                command.config = task.config;
                command.target = run.ref;
                effect.emplace(SessionEffect{taskId, expected, run, std::move(command)});
            } catch (const ToolError& error) {
                fail(error);
            }
        }
        transaction.commit();
    }
    if (!effect) return advance;
    const auto checkpoint = effect->checkpoint();
    const auto& run = checkpoint.at("run");

    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        try {
            admit(effect->taskId, effect->generation, &effect->run);
        } catch (const ToolError& error) {
            fail(error);
            transaction.commit();
            return advance;
        }
        const Resource resource{Action::t3_send,
                                Resource::sessionRead(effect->command.environmentId, effect->command.target.threadId).target};
        auto origin = policyOrigin(message);
        origin.task = effect->taskId;
        origin.generation = effect->generation;
        origin.revision = ownTask(effect->taskId, message).currentRevision.value_or(0);
        const auto gate = approveAction(origin, resource, key, Json::array({decision.tool, args, run}), policyClock());
        if (gate != ApprovalGate::allow) {
            if (gate != ApprovalGate::waiting) fail(ToolError(std::string(approvalError(gate))));
            transaction.commit();
            return gate == ApprovalGate::waiting ? waiting : advance;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",
             {checkpoint, id, step});
        if (!sqlite3_changes(db)) return stop;
        transaction.commit();
    }

    Json outcome;
    try {
        const auto& cmd = effect->command;
        const auto target = Resource::sessionRead(cmd.environmentId, cmd.target.threadId).target;
        outcome = Policy(*this).forbidden(message.bot, Action::t3_send, target) ? toolFailure(approvalError(ApprovalGate::denied))
                                                                                : effect->dispatch(t3);
    } catch (const std::exception& error) {
        const std::string detail = "T3 may have received the interrupt: ";
        outcome = {{"ok", false}, {"taskId", effect->taskId}, {"delivery", "unknown"}, {"error", detail + error.what()}};
    }
    outcome = redactor(message.bot).record(std::move(outcome));
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {id, step}).next()) return stop;
    if (outcome.value("ok", false)) journalInterrupt(effect->taskId, {id, step, effect->run});
    saveTurnResults(id, step, Json::array({outcome}));
    transaction.commit();
    return advance;
}

namespace {
bool acceptedSessionSend(const SessionTarget& target, const Delivery& receipt) {
    return receipt.kind == Delivery::Kind::Accepted && receipt.run.threadId == target.threadId && !receipt.run.runId.empty() &&
           (receipt.delivery == "started" || receipt.delivery == "queued");
}
}

bool Store::Impl::recordSessionSendRun(Id message, const SessionSendReceipt& receipt, std::optional<Seconds> interruptAt) {
    const auto& target = receipt.target;
    const auto& delivery = receipt.delivery;
    if (!acceptedSessionSend(target, delivery)) return false;
    exec("INSERT OR IGNORE INTO session_message_waits(message_id,bot,environment,thread,run,title,notified,gateway_environment,"
         "task_id,action_id,delivery,link,interrupt_at) VALUES(?,?,?,?,?,?,?,?,NULLIF(?,0),NULLIF(?,''),?,?,?)",
         {message, receipt.bot, target.environmentId, target.threadId, delivery.run.runId, receipt.title,
          interruptAt || !receipt.notify || sessionParticipationStopped(receipt.bot, target, message) ? 1 : 0, target.gatewayEnvironmentId,
          receipt.taskId, receipt.actionId, delivery.delivery == "queued" ? "queued" : "started",
          redactor(receipt.bot).text(delivery.run.link), interruptAt ? Json(*interruptAt) : Json(nullptr)});
    if (!receipt.taskId || receipt.actionId.empty()) return true;
    auto task = load(receipt.taskId);
    if (terminal(task)) return true;
    auto& run = destinationRun(task, receipt.actionId);
    auto report = query(R"SQL(
SELECT 1 FROM journal WHERE task_id=? AND kind='report'
AND run_environment_id=? AND run_id=? AND run_thread_id=? LIMIT 1)SQL",
                        {receipt.taskId, run.environmentId, run.ref.runId, run.ref.threadId});
    if ((!run.outstanding() || report.next()) && advanceDestinationSessionMessageRun(task, run)) save(task);
    return true;
}

void Store::Impl::saveSessionSendResult(std::string_view bot, Id message, int step, Json result) {
    const bool unknown = field(result, "delivery") == "unknown";
    const auto results = Json::array({redactor(bot).record(std::move(result))});
    exec(R"SQL(
UPDATE conversation_steps
SET result=?,effect=CASE WHEN ? AND json_extract(effect,'$.taskId') IS NOT NULL THEN effect END
WHERE message_id=? AND step=? AND result IS NULL
)SQL",
         {results, unknown ? 1 : 0, message, step});
}

void Store::Impl::persistSessionSendReceipt(Id message, const SessionSendReceipt& receipt, std::optional<Seconds> interruptAt) {
    const auto& target = receipt.target;
    const auto& delivery = receipt.delivery;
    const bool accepted = recordSessionSendRun(message, receipt, interruptAt);
    const bool settled = accepted || delivery.definitelyUnsent || (interruptAt && delivery.kind == Delivery::Kind::NotAccepted);
    if (settled) {
        exec("DELETE FROM session_message_sends WHERE action=? AND message_id=? AND coalesce(step,-1)=?",
             {receipt.requestId, message, receipt.step});
        if (sqlite3_changes(db) && receipt.step >= 0) {
            exec(R"SQL(
UPDATE conversation_steps SET effect=NULL WHERE message_id=? AND step=?
  AND json_extract(effect,'$.kind')='message_session'
  AND json_extract(effect,'$.sendState')='sending' AND json_extract(effect,'$.requestId')=?
)SQL",
                 {message, receipt.step, receipt.requestId});
        }
    }
    if (receipt.step < 0 || interruptAt) return;
    Json result;
    if (accepted) {
        result = {{"ok", true}, {"delivery", delivery.delivery}};
        if (receipt.notify) result.update({{"environmentId", target.environmentId}, {"threadId", target.threadId}});
    } else {
        result = toolFailure(delivery.detail.empty() ? "Could not confirm delivery to T3" : delivery.detail);
        if (!settled) result["delivery"] = "unknown";
    }
    saveSessionSendResult(receipt.bot, message, receipt.step, std::move(result));
}

void Store::Impl::stopPausedBotParticipation(std::string_view bot) {
    const auto now = policyClock();
    exec("UPDATE session_message_waits SET notified=1,interrupt_at=coalesce(interrupt_at,?) WHERE bot=? AND run_finished=0", {now, bot});
    exec(R"SQL(
UPDATE session_message_sends SET interrupt_at=? WHERE interrupt_at IS NULL
AND message_id IN (SELECT id FROM messages WHERE bot_id=?)
)SQL",
         {now, bot});
}

void Store::Impl::reconcilePausedSessionSends(T3& t3, std::stop_token shutdown) {
    std::vector<std::pair<Id, Command>> pending;
    {
        std::lock_guard lock(mutex);
        auto rows = query(R"SQL(
SELECT s.message_id,m.bot_id,s.environment,s.thread,s.gateway_environment,s.action,s.instruction
FROM session_message_sends s JOIN messages m ON m.id=s.message_id
WHERE s.interrupt_at<=? ORDER BY s.interrupt_at,s.rowid LIMIT 20
)SQL",
                          {policyClock()});
        while (rows.next()) {
            Command command;
            command.tool = CommandTool::message_session;
            command.ownerBotId = rows.string(1);
            command.environmentId = rows.string(2);
            command.target.threadId = rows.string(3);
            command.gatewayEnvironmentId = rows.string(4);
            command.target.gatewayEnvironmentId = command.gatewayEnvironmentId;
            command.actionId = rows.string(5);
            command.instruction = rows.string(6);
            pending.emplace_back(rows.number(0), std::move(command));
        }
    }
    for (const auto& [message, command] : pending) {
        if (shutdown.stop_requested()) return;
        Delivery delivery;
        try {
            delivery = t3.reconcile(command);
        } catch (const std::exception&) {
        }
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        const auto now = policyClock();
        auto row = query(R"SQL(
SELECT coalesce(s.step,-1),json_extract(step.effect,'$.taskId'),json_extract(step.effect,'$.actionId')
FROM session_message_sends s LEFT JOIN conversation_steps step
  ON step.message_id=s.message_id AND step.step=s.step
  AND json_extract(step.effect,'$.kind')='message_session'
  AND json_extract(step.effect,'$.sendState')='sending'
  AND json_extract(step.effect,'$.requestId')=s.action
WHERE s.action=? AND s.message_id=? AND s.interrupt_at<=?
)SQL",
                         {command.actionId, message, now});
        if (!row.next()) {
            transaction.commit();
            continue;
        }
        const SessionTarget target{command.environmentId, command.target.threadId, command.gatewayEnvironmentId};
        persistSessionSendReceipt(message,
                                  {command.ownerBotId,
                                   target,
                                   {},
                                   std::move(delivery),
                                   static_cast<int>(row.number(0)),
                                   row.number(1),
                                   row.string(2),
                                   command.actionId,
                                   false},
                                  now);
        exec("UPDATE session_message_sends SET interrupt_at=? WHERE action=?", {now + 5, command.actionId});
        transaction.commit();
    }
}

void Store::Impl::interruptPausedSessionRuns(T3& t3, std::stop_token shutdown) {
    reconcilePausedSessionSends(t3, shutdown);
    struct Pending {
        Id key;
        std::string environment;
        RunRef run;
    };
    std::vector<Pending> pending;
    {
        std::lock_guard lock(mutex);
        auto rows = query("SELECT rowid,environment,thread,run,gateway_environment FROM session_message_waits "
                          "WHERE interrupt_at<=? ORDER BY interrupt_at,rowid",
                          {policyClock()});
        while (rows.next()) pending.push_back({rows.number(0), rows.string(1), {rows.string(2), rows.string(3), {}, rows.string(4)}});
    }
    for (const auto& item : pending) {
        if (shutdown.stop_requested()) return;
        auto outcome = InterruptOutcome::NoActiveRun;
        try {
            outcome = t3.interruptSessionMessage(item.environment, item.run);
        } catch (const std::exception&) {
        }
        const bool missing = outcome == InterruptOutcome::TargetGone;
        const bool finished = outcome == InterruptOutcome::AlreadyTerminal || missing;
        std::lock_guard lock(mutex);
        exec(R"SQL(
UPDATE session_message_waits
SET interrupt_at=?,run_finished=?,session_missing=max(session_missing,?)
WHERE rowid=? AND interrupt_at IS NOT NULL AND environment=? AND gateway_environment=? AND thread=? AND run=?
)SQL",
             {finished ? Json(nullptr) : Json(policyClock() + 5), finished ? 1 : 0, missing ? 1 : 0, item.key, item.environment,
              item.run.gatewayEnvironmentId, item.run.threadId, item.run.runId});
    }
}

Store::Impl::CallOutcome Store::Impl::sendSessionMessage(const conversation::Message& message, int step, const Decision& decision,
                                                         std::string_view key, const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto id = message.id;
    const bool fixesPullRequest = decision.tool == "fix_pull_request";
    const auto save = [&](Json result) { saveTurnResults(id, step, Json::array({redactor(message.bot).record(std::move(result))})); };
    Command command;
    command.tool = CommandTool::message_session;
    command.actionId = "flock-turn-" + actionId();
    command.ownerBotId = message.bot;
    const bool savedTaskTarget = decision.tool == "send_to_task";
    Id savedTaskId = 0, savedGeneration = 0;
    std::string savedActionId;
    bool includesSavedContent = false;
    std::optional<GitPrFollowup> followup;
    SessionTarget target;
    Resource resource{Action::t3_send, {}};
    constexpr auto stopped = "This Bot's participation in the Session has stopped";
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        auto row = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {id, step});
        if (!row.next()) return stop;
        if (!row.string(0).empty()) {
            if (const auto receipt = sessionSendReceipts.find(id); receipt != sessionSendReceipts.end()) {
                persistSessionSendReceipt(id, receipt->second);
            } else if (field(row.json(0), "sendState") == "prepared") {
                save({{"ok", false}, {"delivery", "not_accepted"}, {"error", "The T3 Session message was not sent."}});
            } else {
                saveSessionSendResult(message.bot, id, step,
                                      {{"ok", false},
                                       {"delivery", "unknown"},
                                       {"error", "Flock restarted while sending this. Check the Session before sending again."}});
            }
            transaction.commit();
            sessionSendReceipts.erase(id);
            return advance;
        }
        if (ctx.hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            save(webEvidenceActionDenied());
            transaction.commit();
            return advance;
        }
        try {
            std::string expectedCommit;
            Json source = nullptr;
            if (fixesPullRequest) {
                followup = gitPrFollowup(message, ctx.intake);
                if (!followup || followup->status != GitPrFollowup::Status::available)
                    throw ToolError("Automatic follow-up is unavailable for this pull request");
                command.environmentId = followup->session.environmentId;
                command.gatewayEnvironmentId = followup->session.gatewayEnvironmentId;
                command.target.threadId = followup->session.threadId;
                command.target.gatewayEnvironmentId = followup->session.gatewayEnvironmentId;
                command.instruction = gitPrFollowupInstruction(message, *followup, decision);
            } else if (savedTaskTarget) {
                const auto task = ownTask(conversation_storage::identifier(decision.args, "taskId", true), message);
                if (conversation_storage::identifier(decision.args, "generation", true, 0) != task.generation)
                    throw ToolError("stale", {{"taskId", task.id}, {"current", taskState(task, message)}});
                if (terminal(task) || task.state == TaskState::cancelling)
                    throw ToolError("The task is " + std::string(taskStateName(task.state)), {{"taskId", task.id}});
                const auto& selected = selectSavedDestination(task, decision.args, true);
                if (const auto sourceId = conversation_storage::identifier(decision.args, "sourceResultMessageId", false); sourceId) {
                    source = resultReference(sourceId, message);
                    if (source.at("taskId") != task.id || source.at("revisionId") != task.currentRevision.value_or(0))
                        throw ToolError("sourceResultMessageId must be a result of this task's current revision");
                }
                command.environmentId = selected.environmentId;
                command.gatewayEnvironmentId = selected.ref.gatewayEnvironmentId;
                command.target.threadId = selected.ref.threadId;
                command.target.gatewayEnvironmentId = selected.ref.gatewayEnvironmentId;
                if (command.environmentId.empty() || command.target.threadId.empty())
                    throw ToolError("The destination Session is unavailable");
                savedTaskId = task.id;
                savedGeneration = task.generation;
                savedActionId = selected.actionId;
                expectedCommit = task.requestedCommit.value_or("");
                command.instruction = conversation_storage::text(decision.args, "instruction", limits::messages::maxBodyBytes, true);
            } else {
                command.environmentId = conversation_storage::text(decision.args, "environmentId", 256, true);
                command.target.threadId = conversation_storage::text(decision.args, "threadId", 256, true);
                command.instruction = conversation_storage::text(decision.args, "text", limits::messages::maxBodyBytes, true);
            }
            const auto name = message.routing.at("config").value("botDisplayName", message.bot);
            command.instruction = name + ": " + command.instruction;
            includesSavedContent = !source.is_null() || !expectedCommit.empty();
            if (!source.is_null())
                command.instruction +=
                    "\n\n(reference only):\n" + bounded(field(source, "text"), limits::session_prompt::maxQuoteTextBytes);
            if (!expectedCommit.empty()) command.instruction = commit_check::instructions(std::move(command.instruction), expectedCommit);
            command.instruction = redactor(message.bot).text(command.instruction);
            if (decision.tool == "message_session" &&
                (decision.args.contains("taskId") || decision.args.contains("generation") || decision.args.contains("runId")))
                throw ToolError("Use the Session identifiers");
            target = {command.environmentId, command.target.threadId, command.gatewayEnvironmentId};
            resource = {Action::t3_send, Resource::sessionRead(target.environmentId, target.threadId).target};
            if (sessionParticipationStopped(message.bot, target, id)) throw ToolError(stopped);
            if (Policy(*this).forbidden(message.bot, resource.action, resource.target))
                throw ToolError(std::string(approvalError(ApprovalGate::denied)));
            if (!fixesPullRequest && !savedTaskTarget && !connectedSessionAccess(message, target))
                throw ToolError("This Session is unavailable through your T3 connection");
        } catch (const ToolError& failure) {
            save(toolFailure(failure.what()));
            transaction.commit();
            return advance;
        }
        transaction.commit();
    }
    const auto availableFollowup = [&]() -> std::optional<GitPrFollowup> {
        auto current = gitPrFollowup(message, ctx.intake);
        if (!current || current->operation != followup->operation || current->session != target ||
            current->status != GitPrFollowup::Status::available)
            return std::nullopt;
        return current;
    };
    std::string error;
    std::string title;
    try {
        command.authGeneration =
            ctx.t3.authGeneration(target.gatewayEnvironmentId.empty() ? target.environmentId : target.gatewayEnvironmentId, command.config);
        if (!fixesPullRequest && !savedTaskTarget) {
            if (const auto metadata = ctx.t3.sessionMetadata(target, false))
                title = metadata->title;
            else
                error = "This Session is unavailable through your T3 connection";
        }
    } catch (const std::exception&) {
        error = "Could not verify access to this T3 Session";
    }
    if (error.empty() && followup && followup->target.contains("headSha") && (!ctx.gitHost || !currentGitPrCheck(*ctx.gitHost, message)))
        error = "Could not confirm that the failing check belongs to this pull request's current head";
    const SessionDispatchFileLock dispatch(path, SessionDispatchFileLock::Mode::shared);
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (ctx.shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        if (sessionParticipationStopped(message.bot, target, id)) {
            save(toolFailure(stopped));
            transaction.commit();
            return advance;
        }
        if (!error.empty() || (!fixesPullRequest && !savedTaskTarget && !connectedSessionAccess(message, target))) {
            save(toolFailure(error.empty() ? "This Session is unavailable through your T3 connection" : error));
            transaction.commit();
            return advance;
        }
        auto origin = policyOrigin(message);
        auto approvalOperation = std::string(key);
        Json policyPayload{decision.tool, decision.args, command.authGeneration};
        if (fixesPullRequest) {
            const auto current = availableFollowup();
            if (!current) {
                save(toolFailure("Automatic follow-up is unavailable for this pull request"));
                transaction.commit();
                return advance;
            }
            origin = current->origin;
            policyPayload = {{"pullRequest", current->target.at("url")},
                             {"diagnosis", decision.args.at("diagnosis")},
                             {"arguments", Json::array({command.instruction, command.authGeneration, target.gatewayEnvironmentId})}};
        } else if (savedTaskTarget) {
            try {
                const auto task = ownTask(savedTaskId, message);
                if (task.generation != savedGeneration)
                    throw ToolError("stale", {{"taskId", savedTaskId}, {"current", taskState(task, message)}});
                if (terminal(task) || task.state == TaskState::cancelling)
                    throw ToolError("The task is " + std::string(taskStateName(task.state)), {{"taskId", savedTaskId}});
                const auto& selected = selectSavedDestination(task, decision.args, true);
                if (selected.actionId != savedActionId || selected.environmentId != target.environmentId ||
                    selected.ref.threadId != target.threadId || selected.ref.runId.empty())
                    throw ToolError("The destination Session is unavailable");
                const auto action = outbox(task, selected.actionId);
                if (!action || !action->destinationAction || action->phase != OutboxPhase::accepted || action->stop ||
                    action->command.environmentId != selected.environmentId || action->command.projectId != selected.assignment.projectId ||
                    action->command.gatewayEnvironmentId != selected.ref.gatewayEnvironmentId)
                    throw ToolError("The destination Session is unavailable");
                if (!query("SELECT 1 FROM runs WHERE action_id=? AND task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                           {selected.actionId, savedTaskId, selected.environmentId, selected.ref.threadId, selected.ref.runId})
                         .next())
                    throw ToolError("The destination Session is unavailable");
                origin.task = savedTaskId;
                origin.generation = savedGeneration;
                origin.revision = task.currentRevision.value_or(0);
                policyPayload.push_back(selected.actionId);
                if (includesSavedContent) {
                    approvalOperation += "/rendered-session-message";
                    policyPayload.push_back(command.instruction);
                }
            } catch (const ToolError& failure) {
                save(toolFailure(failure.what()));
                transaction.commit();
                return advance;
            }
        }
        const auto gate = approveAction(origin, resource, approvalOperation, policyPayload, policyClock());
        if (gate != ApprovalGate::allow) {
            if (gate != ApprovalGate::waiting) save(toolFailure(approvalError(gate)));
            transaction.commit();
            return gate == ApprovalGate::waiting ? waiting : advance;
        }
        Json effect{{"kind", "message_session"}, {"sendState", "prepared"}};
        if (savedTaskTarget) {
            effect["taskId"] = savedTaskId;
            effect["actionId"] = savedActionId;
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? "
             "AND result IS NULL AND effect IS NULL",
             {effect, id, step});
        if (!sqlite3_changes(db)) return stop;
        const auto read = Resource::sessionRead(target.environmentId, target.threadId);
        if (!message.channelScope.empty() || !Policy(*this).allows(message.bot, read)) title.clear();
        title = bounded(redactT3Tokens(redactor(message.bot).text(title)), 512);
        exec(R"SQL(
INSERT INTO session_participations(bot_id,environment_id,thread_id,person_id,state)
SELECT ?,?,?,person_id,'active' FROM t3_connector WHERE singleton=1
ON CONFLICT(bot_id,environment_id,thread_id) DO NOTHING
)SQL",
             {message.bot, target.environmentId, target.threadId});
        transaction.commit();
    }

    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (ctx.shutdown.stop_requested()) return stop;
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        auto row = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {id, step});
        if (!row.next()) {
            transaction.commit();
            return stop;
        }
        auto effect = row.json(0);
        if (effect.value("sendState", std::string{}) != "prepared") {
            transaction.commit();
            return stop;
        }
        if (fixesPullRequest) {
            const auto current = availableFollowup();
            if (!current) {
                save(toolFailure("Automatic follow-up is unavailable for this pull request"));
                transaction.commit();
                return advance;
            }
            recordGitPrFollowup(message, *current);
        }
        effect["sendState"] = "sending";
        effect["requestId"] = command.actionId;
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? AND result IS NULL", {effect, id, step});
        exec(R"SQL(
INSERT INTO session_message_sends(action,message_id,environment,thread,gateway_environment,instruction,step)
VALUES(?,?,?,?,?,?,?)
)SQL",
             {command.actionId, id, target.environmentId, target.threadId, target.gatewayEnvironmentId, command.instruction, step});
        transaction.commit();
    }

    Delivery receipt;
    try {
        receipt = Policy(*this).forbidden(message.bot, resource.action, resource.target)
                      ? Delivery{Delivery::Kind::NotAccepted, {}, std::string(approvalError(ApprovalGate::denied)), true}
                      : ctx.t3.send(command);
    } catch (const std::exception&) {
        receipt.detail = "Could not confirm delivery to T3";
    }
    std::lock_guard lock(mutex);
    // If SQLite is busy, retry the receipt write without repeating the T3 send.
    const auto checkpoint = sessionSendReceipts
                                .insert_or_assign(id, SessionSendReceipt{message.bot, target, std::move(title), std::move(receipt), step,
                                                                         savedTaskId, savedActionId, command.actionId, !fixesPullRequest})
                                .first;
    Transaction transaction(db);
    persistSessionSendReceipt(id, checkpoint->second);
    transaction.commit();
    sessionSendReceipts.erase(checkpoint);
    return advance;
}

Store::Impl::CallOutcome Store::Impl::cancelDestinationRun(const conversation::Message& message, int step, const Decision& decision,
                                                           std::string_view key, bool offered, bool hasWebEvidence,
                                                           std::stop_token shutdown) {
    using enum CallOutcome;
    if (shutdown.stop_requested()) return stop;
    std::optional<SessionDispatchFileLock> dispatch;
    try {
        dispatch.emplace(path, SessionDispatchFileLock::Mode::exclusive);
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (authorizeTurn(message)) saveTurnResults(message.id, step, Json::array({toolFailure(error.what())}));
        transaction.commit();
        return advance;
    }
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    if (!authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    const auto save = [&](Json result) {
        saveTurnResults(message.id, step, Json::array({redactor(message.bot).record(std::move(result))}));
    };
    const auto fail = [&](const ToolError& error) {
        auto result = error.details;
        result["ok"] = false;
        result["error"] = error.what();
        save(std::move(result));
    };
    auto existing = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL", {message.id, step});
    if (!existing.next()) {
        transaction.commit();
        return stop;
    }
    if (!existing.string(0).empty()) {
        save({{"ok", true},
              {"effect", "stop_requested"},
              {"delivery", "unknown"},
              {"error", "The saved stop request is still being reconciled in T3"}});
        transaction.commit();
        return advance;
    }
    if (!offered)
        save(toolFailure("The cancellation action was not offered"));
    else if (hasWebEvidence && !allowedAfterWebEvidence(decision))
        save(webEvidenceActionDenied());
    else
        try {
            if (!decision.args.is_object()) throw ToolError("Tool arguments must be an object");
            const auto taskId = identifier(decision.args, "taskId", true);
            const auto generation = identifier(decision.args, "generation", true, 0);
            const auto task = ownTask(taskId, message);
            if (generation != task.generation) throw ToolError("stale", {{"taskId", taskId}, {"current", taskState(task, message)}});
            const auto& selected = selectSavedDestination(task, decision.args, false);
            auto action = outbox(task, selected.actionId);
            if (!action || !action->destinationAction || action->command.environmentId != selected.environmentId ||
                action->command.projectId != selected.assignment.projectId)
                throw ToolError("The destination is unavailable");
            if (!selected.outstanding()) {
                save({{"ok", true}, {"effect", "already_settled"}, {"taskId", taskId}, {"destination", selected.name}});
            } else if (task.state == TaskState::cancelling) {
                throw ToolError("The whole task is already stopping", {{"taskId", taskId}});
            } else if (terminal(task)) {
                throw ToolError("The task is " + std::string(taskStateName(task.state)), {{"taskId", taskId}});
            } else if (action->stop) {
                save({{"ok", true}, {"effect", "already_requested"}, {"taskId", taskId}, {"destination", selected.name}});
            } else {
                const auto phase = action->phase;
                if (phase != OutboxPhase::ready && phase != OutboxPhase::started && phase != OutboxPhase::accepted)
                    throw ToolError("The destination is unavailable");
                const bool hasRun = !selected.ref.threadId.empty() && !selected.ref.runId.empty();
                const bool exactRun =
                    phase == OutboxPhase::accepted && hasRun &&
                    query("SELECT 1 FROM runs WHERE action_id=? AND task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                          {selected.actionId, taskId, selected.environmentId, selected.ref.threadId, selected.ref.runId})
                        .next();
                if (phase == OutboxPhase::accepted && !exactRun) throw ToolError("The destination Session is unavailable");
                const bool recoveredRun = phase == OutboxPhase::started && hasRun;
                if (exactRun || recoveredRun) {
                    const Resource resource{Action::t3_send, Resource::sessionRead(selected.environmentId, selected.ref.threadId).target};
                    auto origin = policyOrigin(message);
                    origin.task = taskId;
                    origin.generation = generation;
                    origin.revision = task.currentRevision.value_or(0);
                    const auto gate = approveAction(origin, resource, key,
                                                    Json::array({decision.tool, decision.args, runJson(selected.ref)}), policyClock());
                    if (gate != ApprovalGate::allow) {
                        if (gate != ApprovalGate::waiting) fail(ToolError(std::string(approvalError(gate))));
                        transaction.commit();
                        return gate == ApprovalGate::waiting ? waiting : advance;
                    }
                }
                OutboxStop stop{.operation = std::string(key)};
                if (!selected.ref.runId.empty()) stop.run = OutboxRunIdentity{selected.ref.threadId, selected.ref.runId};
                if (phase == OutboxPhase::started && !recoveredRun) {
                    const auto origin = policyOrigin(message);
                    stop.approval = OutboxStopApproval{.decision = {decision.tool, decision.args, {}},
                                                       .generation = generation,
                                                       .revisionId = task.currentRevision.value_or(0),
                                                       .scope = origin.scope,
                                                       .requester = origin.requester,
                                                       .anchor = origin.anchor,
                                                       .turn = 0,
                                                       .background = origin.background};
                }
                action->stop = std::move(stop);
                if (!saveOutbox(*action, phase)) throw ToolError("The destination changed before its stop request was saved");
                exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=? AND result IS NULL AND effect IS NULL",
                     {Json{{"kind", "destination_cancel"},
                           {"taskId", taskId},
                           {"generation", generation},
                           {"actionId", selected.actionId},
                           {"environmentId", selected.environmentId},
                           {"projectId", selected.assignment.projectId},
                           {"run", runJson(selected.ref)}},
                      message.id, step});
                if (!sqlite3_changes(db)) throw ToolError("The stop request could not be saved");
                save({{"ok", true}, {"effect", "stop_requested"}, {"taskId", taskId}, {"destination", selected.name}});
            }
        } catch (const ToolError& error) {
            fail(error);
        }
    transaction.commit();
    return advance;
}

void Store::Impl::wakeSessionMessages(T3& t3, std::stop_token shutdown) {
    struct Wait {
        Id key, message;
        std::string bot;
        SessionTarget target;
        std::string run, title;
    };
    std::vector<Wait> waits;
    {
        std::lock_guard lock(mutex);
        auto rows = query("SELECT rowid,message_id,bot,environment,thread,run,title,gateway_environment "
                          "FROM session_message_waits WHERE notified=0 ORDER BY rowid");
        while (rows.next())
            waits.push_back({rows.number(0),
                             rows.number(1),
                             rows.string(2),
                             {rows.string(3), rows.string(4), rows.string(7)},
                             rows.string(5),
                             rows.string(6)});
    }
    for (const auto& wait : waits) {
        if (shutdown.stop_requested()) return;
        const SessionDispatchFileLock dispatch(path, SessionDispatchFileLock::Mode::shared);
        const auto authorized = [&] {
            if (botPaused(wait.bot) || !query("SELECT 1 FROM session_message_waits WHERE rowid=? AND notified=0", {wait.key}).next())
                return false;
            const auto message = loadTurnMessage(wait.message);
            return !sessionParticipationStopped(wait.bot, wait.target, wait.message) &&
                   (message.principalSource != conversation::PrincipalSource::session || activeSessionOrigin(message)) &&
                   connectedSessionAccess(message, wait.target);
        };
        // A revoked connection settles the wait silently, including on restart.
        const auto settle = [&](bool finished = false) {
            exec("UPDATE session_message_waits SET notified=1,run_finished=? WHERE rowid=? AND notified=0", {finished ? 1 : 0, wait.key});
        };
        {
            std::lock_guard lock(mutex);
            if (!authorized()) {
                settle();
                continue;
            }
        }
        const auto reason = t3.sessionMessageWake(wait.target, {wait.target.threadId, wait.run, {}, wait.target.gatewayEnvironmentId});
        if (reason.empty() || shutdown.stop_requested()) continue;
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorized()) {
            settle();
            transaction.commit();
            continue;
        }
        settle(reason == "finished" || reason == "failed");
        if (!sqlite3_changes(db)) continue;
        const auto source = loadTurnMessage(wait.message);
        auto routing = source.routing;
        routing.erase("sessionInvitation");
        Policy policy(*this);
        auto title = wait.title;
        if (!policy.allows(wait.bot, Resource::sessionRead(wait.target.environmentId, wait.target.threadId))) title.clear();
        if (auto origin = routing.find("sessionOrigin");
            origin != routing.end() &&
            !policy.allows(wait.bot, Resource::sessionRead(field(*origin, "environmentId"), field(*origin, "threadId"))))
            (*origin)["title"] = "";
        const ConversationWakeEntry wake{wait.target.environmentId, wait.target.threadId, std::move(title), reason, wait.message};
        routing["sessionWake"] = encodeConversationWake(wake);
        const auto id = journalConversationMessage(wait.bot, {wake});
        exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,'',?,?,?,? FROM messages WHERE bot_id=?
)SQL",
             {id, wait.bot, "session-wake:" + std::to_string(wait.key), wait.message, routing, source.admittedActor.id,
              source.admittedActor.adminRoot ? 1 : 0, static_cast<int>(source.principalSource), wait.bot});
        exec(R"SQL(
INSERT INTO channel_messages(message_id,event_key,scope_key,origin,sender_id,person_id)
SELECT ?,?,scope_key,origin,sender_id,person_id FROM channel_messages WHERE message_id=?
)SQL",
             {id, "session-wake:" + std::to_string(wait.key), wait.message});
        exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')", {id});
        transaction.commit();
    }
}

namespace {
void rememberSourceResult(TaskData& task, const OutboxSource& source) {
    auto& refs = task.sourceResultRefs;
    if (std::ranges::find(refs, source.messageId, &OutboxSource::messageId) == refs.end()) refs.push_back(source);
}
}

Id Store::Impl::startConversationTask(const Json& routing, std::string_view bot, std::string_view request, const TaskRoute& destination,
                                      Id messageId, const std::optional<OutboxSource>& reference, bool resume,
                                      const WorkSession* selectedSession) {
    if (selectedSession) redactor(bot).requirePublic(selectedSession->target.threadId);
    const auto id = submit({std::string(bot), std::string(request), destination.environmentId, destination.projectId, routing.at("config")},
                           messageId, reference ? &*reference : nullptr);
    auto task = load(id);
    if (!destination.gatewayEnvironmentId.empty() && destination.gatewayEnvironmentId != destination.environmentId)
        task.gatewayEnvironmentId = destination.gatewayEnvironmentId;
    if (const auto origin = routing.find("sessionOrigin"); origin != routing.end())
        task.sessionOrigin = decodeSessionOrigin(*origin, {"messages", std::to_string(messageId), "routing.sessionOrigin"});
    else if (resume) {
        auto parent = query(R"SQL(
SELECT t.origin_environment_id,t.origin_thread_id,t.origin_gateway_environment_id,t.origin_title FROM tasks t
JOIN messages m ON m.id=t.source_message_id
JOIN messages request ON request.id=? AND request.person_id=m.person_id
LEFT JOIN channel_messages channel ON channel.message_id=request.id
WHERE t.id=? AND t.bot_id=? AND t.origin_environment_id IS NOT NULL AND channel.message_id IS NULL
)SQL",
                            {messageId, reference.value().taskId, bot});
        if (parent.next()) task.sessionOrigin = SessionOrigin{{parent.string(0), parent.string(1), parent.string(2)}, parent.string(3)};
    }
    if (task.sessionOrigin) task.sessionOrigin->title = redactor(bot).text(task.sessionOrigin->title);
    if (selectedSession)
        task.selectedSession = WorkSession{
            {destination.environmentId, selectedSession->target.threadId, task.gatewayEnvironmentId.value_or("")}, destination.projectId};
    // Establish provenance at intake, while both the original message and the routed request are available.
    auto message = query("SELECT text FROM messages WHERE id=? AND bot_id=?", {messageId, bot});
    task.sourceMessageCoversFullRequest = !reference && message.next() && message.string(0) == request;
    if (resume) {
        const auto priorId = reference.value().taskId;
        const auto prior = load(priorId);
        const auto criteria = effectiveDoneWhen(prior);
        if (!criteria.empty()) task.criteriaProposal = TaskCriteriaProposal{criteria, std::nullopt};
        const auto& pending = prior.cancelledInstructions;
        std::set<std::pair<Id, int>> pendingIds;
        for (const auto& item : pending) pendingIds.emplace(item.messageId, item.step);

        // Excerpt only earlier deliveries. Pending directions follow them in acceptance order.
        auto rows = query("SELECT id,data FROM journal WHERE task_id=? AND kind='instruction_accepted'"
                          " ORDER BY id DESC",
                          {priorId});
        std::vector<DeliveredInstruction> delivered;
        constexpr std::size_t maxEarlierInstructionBytes = 8'000, maxEarlierInstructions = 20;
        std::size_t remaining = maxEarlierInstructionBytes;
        while (remaining && delivered.size() < maxEarlierInstructions && rows.next()) {
            const auto item = decodeAcceptedInstruction(rows.number(0), rows.json(1));
            if (pendingIds.contains({item.messageId, item.step})) continue;
            auto text = bounded(item.text, remaining);
            remaining -= text.size();
            delivered.push_back({item.messageId, item.step, std::move(text)});
        }
        std::reverse(delivered.begin(), delivered.end());
        task.intent = prior.intent;
        task.restartContext = TaskRestart{priorId, std::move(delivered)};
        for (auto item : pending) {
            item.revisionId.reset();
            item.conflicting = false;
            if (item.source) rememberSourceResult(task, *item.source);
            journal(id, item);
            task.pendingInstructions.push_back(std::move(item));
        }
    }
    save(task);
    return id;
}

bool Store::Impl::acceptInstruction(TaskData& task, Id messageId, int step, std::string_view instruction,
                                    std::optional<OutboxSource> source, const TaskChange* change) {
    Id revision = task.currentRevision.value_or(0);
    bool conflicting = task.replanning;
    if (change) {
        const auto oldIntent = task.intent;
        const auto oldDone = effectiveDoneWhen(task);
        const auto intent = change->intent.empty() ? oldIntent : change->intent;
        const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
        const auto& environment = change->route.environmentId;
        const auto& pendingRoute = task.pendingDestination;
        const auto& currentEnvironment = pendingRoute ? pendingRoute->route.environmentId : task.environmentId;
        const auto& currentProject = pendingRoute ? pendingRoute->route.projectId : task.projectId;
        const auto currentGateway =
            pendingRoute ? pendingRoute->route.gatewayEnvironmentId : task.gatewayEnvironmentId.value_or(currentEnvironment);
        const auto gateway = change->route.gatewayEnvironmentId.empty() ? environment : change->route.gatewayEnvironmentId;
        const bool destinationChanged =
            !environment.empty() &&
            (environment != currentEnvironment || (!change->route.projectId.empty() && change->route.projectId != currentProject) ||
             gateway != currentGateway);
        std::optional<PendingDestination> nextRoute;
        if (destinationChanged) {
            auto project = change->route.projectId;
            const bool pinned = !project.empty();
            if (!pinned) project = rememberedT3Project(task.botId, environment);
            if (recipientRouteWouldCycle(task.id, environment, project)) {
                if (pinned) throw Conflict("This destination would make project queues wait for one another");
                project.clear();
            }
            auto route = change->route;
            route.projectId = std::move(project);
            route.gatewayEnvironmentId = gateway;
            nextRoute = PendingDestination{std::move(route), pinned};
        }
        const bool criteriaChanged = oldDone != done;
        const bool intentChanged = oldIntent != intent;
        if (criteriaChanged || intentChanged) {
            if (task.currentRevision)
                revision = addRevision(task, intent, done, messageId);
            else {
                task.intent = intent;
                if (criteriaChanged) task.criteriaProposal = TaskCriteriaProposal{done, messageId};
                journal(task.id, "task_contract_changed", {{"messageId", messageId}});
            }
            if (intentChanged) task.sourceMessageCoversFullRequest = false;
            conflicting = true;
        }
        if (nextRoute) {
            task.pendingDestination = std::move(nextRoute);
            conflicting = true;
        }
    }
    if (source) rememberSourceResult(task, *source);
    OutboxInstruction pending{messageId, step, std::string(instruction), revision, conflicting, std::move(source)};
    if (conflicting) {
        task.replanning = true;
        releaseModelCall(takeReservedTaskCall(task.eventId));
        if (isUndispatchedNotesWait(task)) {
            clearWait(task, "request_changed");
            task.state = TaskState::queued;
        } else if (task.state == TaskState::waiting && waitIsActionable(task)) {
            auto previous = currentWait(task.id);
            if (!previous) throw DecisionUnavailable{};
            const auto reason = previous->reason();
            if (const auto allowance = waitAllowance(task.id, reason)) previous->allowance = allowance;
            clearWait(task, "request_changed");
            const auto waitId = wait(task, std::move(*previous));
            if (task.question) task.question->target = questionTarget(task, reason == WaitReason::user_question ? waitId : 0);
        }
    }
    if (task.question) task.question->suspendedDecision.reset();
    journal(task.id, pending);
    task.pendingInstructions.push_back(std::move(pending));
    save(task);
    return conflicting;
}

void Store::Impl::recordTaskBlocker(TaskData& task, std::string_view reason, Id waitId) {
    if ((!reason.empty() && !conversation_storage::hasText(reason)) || reason.size() > 2000)
        throw Conflict("Provide a blocker reason of up to 2,000 bytes, or an empty string to clear it");
    if (waitId) {
        const auto wait = currentWait(task.id);
        if (reason.empty() || task.state != TaskState::waiting || !wait || wait->id != waitId || !waitIsActionable(task))
            throw DecisionUnavailable{};
    }
    const auto publicReason = redactor(task.botId).text(std::string(reason));
    if (reason.empty())
        task.blocker.reset();
    else
        task.blocker = RecordedTaskBlocker{publicReason, waitId ? std::optional{waitId} : std::nullopt};
    auto entry = Json{{"blockerReason", publicReason}, {"targetRevision", task.currentRevision.value_or(0)}};
    if (waitId) entry["waitId"] = waitId;
    journal(task.id, reason.empty() ? "blocker_cleared" : "blocker_recorded", entry);
    save(task);
}

void Store::Impl::withdrawTaskQuestion(TaskData& task, Id waitId) {
    const auto wait = currentWait(task.id);
    if (task.state != TaskState::waiting || !wait || wait->reason() != WaitReason::user_question || wait->id != waitId ||
        !waitIsActionable(task))
        throw Conflict("The question is no longer open");
    if (std::holds_alternative<ProjectQuestionWait>(wait->details)) throw Conflict("Choose a T3 project before this task can start");
    auto updated = currentEvent(task);
    for (const auto* metadata : {"id", "failures", "retryAt", "evidence", "consumed"}) updated.erase(metadata);
    updated["withdrawnUserQuestion"] = wait->text;
    consumeEvent(task);
    event(task, "withdraw/" + std::to_string(waitId), updated);
    clearWait(task, "withdrawn");
    if (task.question) task.question->target = questionTarget(task);
    journal(task.id, "question_withdrawn", {{"waitId", waitId}});
    task.state = TaskState::deciding;
    save(task);
}

}
