#include "flock/tasks.hpp"
#include "flock/task_list.hpp"
#include "flock/conversation_intake.hpp"
#include "flock/web_conversation_intake.hpp"
#include "conversation_storage.hpp"
#include "flock/task_control.hpp"
#include "flock/task_history.hpp"
#include "flock/task_intake.hpp"
#include "flock/notifications.hpp"
#include "flock/decision_errors.hpp"
#include "flock/policy.hpp"
#include "flock/peer_conversations.hpp"
#include "flock/channels.hpp"
#include "flock/local_machines.hpp"
#include "flock/identity.hpp"
#include "flock/workflows.hpp"
#include "flock/git_events.hpp"
#include "git_storage.hpp"
#include "flock/bot_memory.hpp"
#include "flock/automations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/conversations.hpp"
#include "channel_storage.hpp"
#include "database.hpp"
#include "memory_storage.hpp"
#include "task_routing.hpp"
#include "task_records.hpp"
#include "task_operations.hpp"
#include "secret_storage.hpp"
#include "peer_storage.hpp"
#include "model_calls.hpp"
#include "bot_lifecycle_storage.hpp"
#include "automation_storage.hpp"
#include "session_stop.hpp"
#include "wait_storage.hpp"
#include "task_events.hpp"
#include "journal_storage.hpp"
#include "storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_peer.hpp"
#include "task_storage.hpp"
#include "task_interaction_storage.hpp"
#include "task_context_storage.hpp"
#include "outbox_storage.hpp"
#include "flock/session_settings.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/t3_command.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"
#include "json_fields.hpp"
#include "flock/secrets.hpp"
#include "run_ref_storage.hpp"
#include "inbox_storage.hpp"
#include "flock/task_target.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
Seconds unixTime() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
std::string bounded(std::string_view text, std::size_t bytes) {
    if (text.size() <= bytes) return std::string(text);
    while (bytes && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) --bytes;
    return std::string(text.substr(0, bytes));
}
std::string sessionPrompt(const std::string& actionId, const std::string& instruction) {
    const auto marker = "\n\n[Flock action ID: " + actionId + "]";
    return instruction.ends_with(marker) ? instruction : instruction + marker;
}

std::string field(const Json& j, std::string_view key) {
    const auto found = j.find(key);
    return found != j.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
Id requiredInteger(const Json& data, std::string_view key, Id minimum) {
    const auto value = data.find(key);
    if (value == data.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()) || value->get<Id>() < minimum)
        throw Conflict("Invalid " + std::string(key));
    return value->get<Id>();
}
std::string actionId() {
    std::random_device random;
    constexpr std::string_view hex = "0123456789abcdef";
    std::string id = "flock-";
    for (int i = 0; i < 16; ++i) {
        const auto octet = random() & 0xff;
        id += hex[octet >> 4];
        id += hex[octet & 15];
    }
    return id;
}
Json runJson(const RunRef& run) {
    Json result{{"threadId", run.threadId}, {"runId", run.runId}, {"link", run.link}};
    if (!run.gatewayEnvironmentId.empty()) result["gatewayEnvironmentId"] = run.gatewayEnvironmentId;
    return result;
}
RunRef runRef(const Json& data) {
    return {field(data, "threadId"), field(data, "runId"), field(data, "link"), field(data, "gatewayEnvironmentId")};
}

Store::Impl::Impl(std::filesystem::path file, MemorySeed seed, AutomationModelResolver model)
    : path(std::move(file)), memoryStorage(database, secretStorage, permissions, conversationTurns, std::move(seed)),
      automationStorage(database, secretStorage, botLifecycle, permissions, std::move(model), taskOperations) {
    database.open(path);
}

Policy::Policy(Store& store) : Policy(store.p_->permissions) {}

void TaskOperations::requestCancellation(TaskData& task, SessionStop stop) {
    const Id id = task.id;
    const auto peer = peerStorage.peerExchange(id);
    const bool undispatched = task.state == TaskState::queued || taskRouting.isUndispatchedNotesWait(task);
    if (!task.pendingInstructions.empty()) task.cancelledInstructions = task.pendingInstructions;
    waitStorage.clearWait(task, "cancel");
    taskEvents.consumeEvent(task);
    journalStorage.journalCancelRequested(id, stop);
    if (undispatched && !peer) {
        task.pendingDestination.reset();
        task.replanning = false;
        task.pendingInstructions.clear();
        task.state = TaskState::cancelled;
        journalStorage.journalCancelled(id);
    } else
        task.state = TaskState::cancelling;
    taskRecords.save(task);
    if (peer && peer->sender == id) {
        auto recipient = taskRecords.load(peer->recipient);
        if (stop.participation && peer->kind == "work") {
            // The Session exit owns this task's Run, while the peer still owns its own task and Run.
            database.exec("UPDATE peer_exchanges SET phase='closed' WHERE id=?", {peer->id});
            if (recipient.state == TaskState::peer_waiting) {
                taskEvents.event(recipient, "peer-closed/" + peer->id, {{"kind", "intake"}});
                recipient.state = TaskState::deciding;
                taskRecords.save(recipient);
            }
        } else if (!terminal(recipient) && recipient.state != TaskState::cancelling)
            requestCancellation(recipient);
    }
}

Store::Store(std::filesystem::path db, MemorySeed seed, AutomationModelResolver automationModel)
    : p_(std::make_unique<Impl>(std::move(db), std::move(seed), std::move(automationModel))) {}
Store::~Store() = default;

void GitEvents::protectGitCredential(std::string value) {
    secrets_.protectGitCredential(std::move(value));
}
Database& Database::of(Store& store) {
    return store.p_->database;
}
Tasks::Tasks(Store& store) : database_(Database::of(store)), records_(store.p_->taskRecords) {}

TaskList::TaskList(Store& store) : database_(Database::of(store)), records_(store.p_->taskRecords), waits_(store.p_->waitStorage) {}

bool Tasks::hasTask(Id id) const {
    std::lock_guard lock(database_.mutex);
    return database_.query("SELECT 1 FROM tasks WHERE id=?", {id}).next();
}

TaskView Tasks::task(Id id) const {
    std::lock_guard lock(database_.mutex);
    return records_.taskDetails(id, true);
}

ConversationIntake::ConversationIntake(Store& store) : turns_(store.p_->conversationTurns) {}

WebConversationIntake::WebConversationIntake(Store& store)
    : database_(Database::of(store)), turns_(store.p_->conversationTurns), permissions_(store.p_->permissions) {}

Json ConversationIntake::receive(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                                 const Json& attachments) {
    return turns_.receive(
        actor, routing.botId, clientId, text, replyTo, conversation::PrincipalSource::trusted, nullptr, [&] { return routing; },
        attachments);
}

Conversations::Conversations(Store& store)
    : database_(Database::of(store)), context_(store.p_->conversationContextStorage), memory_(store.p_->memoryStorage),
      modelCalls_(store.p_->modelCalls), channels_(store.p_->channelStorage) {}

Memory::Memory(Store& store) : database_(Database::of(store)), memory_(store.p_->memoryStorage) {}

Automations::Automations(Store& store)
    : database_(Database::of(store)), automation_(store.p_->automationStorage), lifecycle_(store.p_->botLifecycle),
      secrets_(store.p_->secretStorage), tasks_(store.p_->taskOperations), policy_(store.p_->permissions) {}

Inbox::Inbox(Store& store) : inbox_(store.p_->inboxStorage), git_(store.p_->gitStorage) {}

GitEvents::GitEvents(Store& store) : git_(store.p_->gitStorage), secrets_(store.p_->secretStorage) {}

TaskIntake::TaskIntake(Store& store) : database_(Database::of(store)), operations_(store.p_->taskOperations) {}

TaskControl::TaskControl(Store& store)
    : database_(Database::of(store)), outbox_(store.p_->outboxStorage), operations_(store.p_->taskOperations),
      records_(store.p_->taskRecords), waits_(store.p_->waitStorage) {}

TaskHistory::TaskHistory(Store& store) : database_(Database::of(store)), records_(store.p_->taskRecords) {}

Channels::Channels(Store& store) : channels_(store.p_->channelStorage) {}

LocalMachines::LocalMachines(Store& store) : machines_(store.p_->localMachineStorage) {}

WebIdentity::WebIdentity(Store& store)
    : database_(Database::of(store)), machines_(store.p_->localMachineStorage), identity_(store.p_->webIdentity) {}

Workflows::Workflows(Store& store) : database_(Database::of(store)), workflows_(store.p_->workflowStorage) {}

Notifications::Notifications(Store& store)
    : database_(Database::of(store)), secrets_(store.p_->secretStorage), channels_(store.p_->channelStorage),
      permissions_(store.p_->permissions) {}

PeerConversations::PeerConversations(Store& store) : database_(Database::of(store)) {}

const std::filesystem::path& Store::path() const {
    return p_->path;
}

std::pair<Id, Id> Store::changeStamp() const {
    return p_->database.changeStamp();
}

Conversations::ConversationChanges Conversations::conversationChangesSince(std::optional<Id> after, std::string_view bot) const {
    std::lock_guard lock(database_.mutex);
    auto newest = database_.query("SELECT coalesce(max(id),0) FROM journal");
    newest.next();
    ConversationChanges changes{newest.number(0), {}};
    if (!after || *after >= changes.through) return changes;
    auto rows = database_.query(R"SQL(
SELECT m.bot_id,m.id FROM journal j JOIN messages m ON m.id=CASE j.kind
  WHEN 'conversation_reply' THEN j.reply_to_message_id ELSE j.message_id END
WHERE j.id>?1 AND j.id<=?2 AND (?3='' OR m.bot_id=?3)
  AND j.kind IN ('conversation_reply','instruction_accepted','instruction_delivered',
                 'conversation_budget_approved')
UNION
SELECT j.conversation_bot_id,j.wait_message_id FROM journal j
WHERE j.id>?1 AND j.id<=?2 AND j.kind='conversation_budget_approved'
  AND (?3='' OR j.conversation_bot_id=?3)
ORDER BY 1,2
)SQL",
                                {*after, changes.through, bot});
    while (rows.next()) changes.messages[rows.string(0)].push_back(rows.number(1));
    return changes;
}

Notifications::NotificationEvents Notifications::notificationEventsSince(const Person& viewer, const std::vector<std::string>& bots,
                                                                         std::optional<Id> after,
                                                                         const std::map<std::string, Id>& previousScope) const {
    NotificationEvents result{after.value_or(0), Json::array(), {}, false};
    std::lock_guard lock(database_.mutex);
    Transaction snapshot(database_.db, Transaction::Mode::read);
    std::map<std::string, Id> established, newlyVisible;
    for (const auto& bot : bots) {
        if (!permissions_.allows(viewer, Action::bot_view, bot)) continue;
        const auto previous = previousScope.find(bot);
        if (after && previous != previousScope.end()) {
            result.scope.emplace(bot, previous->second);
            established.emplace(bot, std::max(*after, previous->second));
        } else {
            result.scope.emplace(bot, 0);
            newlyVisible.emplace(bot, result.through);
        }
    }
    constexpr Id pageSize = 100;
    static const std::string notificationEvents = R"SQL(
WITH visible_bots AS (SELECT key AS bot,value AS after FROM json_each(?1)),
events(id,bot,kind) AS (
SELECT j.id,b.bot,
       CASE WHEN j.conversation_route='error' THEN 'failure' ELSE 'reply' END
FROM visible_bots b JOIN journal j ON j.conversation_bot_id=b.bot AND j.id>b.after
WHERE j.kind='conversation_reply' AND j.conversation_route IS NOT 'model_budget'
UNION ALL
SELECT j.id,b.bot,'failure'
FROM visible_bots b JOIN tasks t ON t.bot_id=b.bot JOIN journal j ON j.task_id=t.id AND j.id>b.after
WHERE j.conversation_bot_id IS NULL AND (
  (j.kind='report' AND (
    j.run_status IN ('failed','rolled_back')
    OR ((j.run_status='interrupted' OR (
           j.run_status='cancelled' AND j.stop_requested=0
           AND length(j.run_id)>0 AND length(j.run_thread_id)>0
        )) AND j.stop_requested IS NOT 1
        AND NOT EXISTS (
          SELECT 1 FROM journal stop
          WHERE stop.task_id=j.task_id AND stop.id<j.id AND stop.kind='interrupt_requested'
            AND (stop.run_environment_id,stop.run_thread_id,stop.run_id,stop.run_gateway_id)
            IS (j.run_environment_id,j.run_thread_id,j.run_id,j.run_gateway_id)
        ))
  ))
  OR (j.kind='waiting' AND j.wait_reason='model_failure')
  OR (j.kind='completed' AND j.workflow_outcome='failed')
)
)
)SQL";
    // The global page cursor advances only over events delivered for already visible Bots.
    if (!newlyVisible.empty()) {
        static const auto baselines = notificationEvents + " SELECT bot,max(id) FROM events GROUP BY bot";
        auto rows = database_.query(baselines.c_str(), Json::array({newlyVisible}));
        while (rows.next()) {
            result.scope.at(rows.string(0)) = rows.number(1);
            if (!after) result.through = std::max(result.through, rows.number(1));
        }
    }
    static const auto page = notificationEvents + " SELECT id,bot,kind FROM events ORDER BY id LIMIT ?2";
    auto rows = database_.query(page.c_str(), Json::array({established, pageSize + 1}));
    while (rows.next()) {
        if (result.events.size() == pageSize) {
            result.more = true;
            break;
        }
        result.through = rows.number(0);
        result.events.push_back({{"id", result.through}, {"botId", rows.string(1)}, {"kind", rows.string(2)}});
    }
    return result;
}

std::optional<Notifications::BotViewStamp> Notifications::botViewStamp() const {
    std::lock_guard lock(database_.mutex);
    if (channels_.hasConnectors() || database_.query("SELECT 1 FROM protected_inputs WHERE status='fulfilled' LIMIT 1").next())
        return std::nullopt;
    return BotViewStamp{database_.changeStamp(), secrets_.gitCredentialCount()};
}

Id TaskIntake::submit(const Intake& item, const Person* requester) {
    std::lock_guard lock(database_.mutex);
    Transaction transaction(database_.db);
    const auto id = operations_.submitRequest(item, requester);
    transaction.commit();
    return id;
}

Id TaskOperations::submitRequest(const Intake& item, const Person* requester) {
    if (!requester) return submit(item);
    if (!permissions.allows(*requester, Action::bot_talk, item.botId)) throw DecisionDenied{};
    secretStorage.sharedRedactor().requirePublic(requester->id);
    return submit(item, 0, nullptr, TaskSource::request, {}, requester->id);
}

Id TaskOperations::submit(const Intake& item, Id sourceMessageId, const OutboxSource* source, TaskSource sourceKind,
                          std::optional<TaskTrigger> trigger, std::optional<std::string> requester) {
    const bool consultation = sourceKind == TaskSource::consultation;
    if (item.botId.empty() || item.request.find_first_not_of(" \r\n\t") == std::string::npos ||
        item.request.size() > limits::messages::maxBodyBytes || (!consultation && item.environmentId.empty()) || !item.config.is_object())
        throw Conflict("Bot, nonempty request (up to " + std::to_string(limits::messages::maxBodyBytes) +
                       " bytes), and environment are required");
    if (botLifecycle.botDeleted(item.botId)) throw Conflict("Bot has been deleted");
    if (botLifecycle.botPaused(item.botId)) throw BotPaused{};
    const auto scrub = secretStorage.redactor(item.botId);
    scrub.requirePublic({item.botId, item.environmentId, item.projectId, item.config});
    initialModelCallLimit(item.config);
    const auto request = scrub.text(item.request);
    TaskData task{.state = consultation ? TaskState::deciding : TaskState::queued,
                  .botId = item.botId,
                  .request = request,
                  .intent = request,
                  .environmentId = item.environmentId,
                  .projectId = item.projectId,
                  .doneWhen = {},
                  .createdAt = unixTime(),
                  .allowance = {.remaining = WaitStorage::initialWorkAllowance},
                  .config = item.config};
    if (sourceMessageId) task.sourceMessageId = sourceMessageId;
    if (source) {
        task.sourceTaskId = source->taskId;
        task.sourceResultRefs.push_back(*source);
    }
    task.trigger = std::move(trigger);
    task.requester = std::move(requester);
    const auto id = taskRecords.insertTask(task);
    if (sourceKind == TaskSource::request) {
        Json entry = {{"text", request},
                      {"messageId", sourceMessageId},
                      {"source", sourceMessageId ? "conversation"
                                 : task.trigger  ? "automation"
                                                 : "manual"}};
        if (task.trigger) entry["trigger"] = taskTriggerJson(*task.trigger);
        database.writeJournal(id, "request", entry);
    }
    return id;
}

Json TaskControl::reconciliationData(Id id, Id waitId, Json target, const IntakeResolver& intake) const {
    TaskData task;
    {
        std::lock_guard lock(database_.mutex);
        Transaction transaction(database_.db, Transaction::Mode::read);
        const auto wait = waits_.currentWait(id);
        if (!wait || wait->id != waitId || wait->reason() != WaitReason::settings) return target;
        task = records_.load(id);
        const auto pending = outbox_.outbox(task);
        if (pending && (pending->phase != OutboxPhase::ready || pending->command.tool == CommandTool::start_in_session)) return target;
        transaction.commit();
    }

    const auto current = intake(task.botId, task.request);
    const auto environment = task.environmentId;
    const auto destinations = current.config.value("conversationEnvironments", Json::object());
    const auto project = task.projectId;
    const bool namedRoute = std::ranges::any_of(destinations, [&](const Json& alias) {
        return conversationEnvironmentId(alias) == environment &&
               (environment != current.environmentId || field(alias, "projectId") == project);
    });
    const bool settingsMatch =
        environment == current.environmentId ? (current.projectId.empty() || project == current.projectId) : current.environmentId.empty();
    if (!settingsMatch && !namedRoute) throw Conflict("The project route changed. Cancel this task and submit a new request.");
    target["session"] = current.config.value("session", Json::object());
    return target;
}

TaskTarget TaskTarget::from(const TaskData& task, Id waitId) {
    return {task.botId, task.id, waitId, task.currentRevision.value_or(0), task.generation};
}

void TaskControl::operate(const Person& actor, const TaskTarget& target, std::string_view operation, const Json& data) {
    std::lock_guard lock(database_.mutex);
    Transaction transaction(database_.db);
    operations_.operate(actor, target, operation, data);
    transaction.commit();
}

void TaskOperations::operate(const Person& actor, const TaskTarget& target, std::string_view operation, const Json& input, Id sourceMessage,
                             std::optional<InboxKind> expectedKind) {
    const auto& [bot, id, waitId, revision, generation] = target;
    if (!permissions.allows(actor, Action::bot_decide, bot)) throw DecisionDenied{};
    if (operation != "cancel" && botLifecycle.botPaused(bot)) throw BotPaused{};
    auto task = taskRecords.load(id);
    if (task.botId != bot || task.currentRevision.value_or(0) != revision) throw RequestChanged{};
    const auto data = secretStorage.redactor(bot).payload(input);
    if (!data.is_object()) throw Conflict("Operation data must be an object");
    const auto wait = waitStorage.currentWait(id);
    if (operation == "cancel") {
        if (task.generation != generation) throw RequestChanged{};
        if (terminal(task) || task.state == TaskState::cancelling || (waitId && (!wait || wait->id != waitId))) throw DecisionUnavailable{};
        requestCancellation(task);
        return;
    }
    if (!waitStorage.waitIsActionable(task)) throw RequestChanged{};
    if (!wait || wait->id != waitId) {
        auto prior = database.query("SELECT resolution FROM waits WHERE id=? AND task_id=?", {waitId, id});
        if (!prior.next() || prior.string(0) == "request_changed") throw RequestChanged{};
        throw DecisionUnavailable{};
    }
    if (task.state != TaskState::waiting) throw DecisionUnavailable{};
    const auto reason = wait->reason();
    const auto* verification = wait->verification();
    const bool extendingVerificationDeadline = operation == "extend" && verification;
    if (task.generation != generation && !extendingVerificationDeadline) throw RequestChanged{};
    const auto kind = inboxWaitKind(reason);
    if (expectedKind && kind != expectedKind) throw RequestChanged{};
    auto next = TaskState::deciding;
    if (operation == "answer" && reason == WaitReason::user_question) {
        const auto owner = task.question ? task.question->target : waitQuestionTarget(*wait);
        if (owner != questionTarget(task, waitId)) throw RequestChanged{};
        const auto answer = field(data, "text");
        if (answer.find_first_not_of(" \r\n\t") == std::string::npos || answer.size() > limits::messages::maxWaitAnswerBytes)
            throw Conflict("A nonempty answer (up to " + std::to_string(limits::messages::maxWaitAnswerBytes) + " bytes) is required");
        const UserAnswerEntry entry{answer, waitId, sourceMessage ? std::optional{sourceMessage} : std::nullopt};
        if (const auto* question = std::get_if<ProjectQuestionWait>(&wait->details)) {
            const auto choice = question->projects.find(answer);
            if (choice == question->projects.end()) throw Conflict("Choose one of the listed T3 projects");
            const auto environment = task.environmentId;
            const auto& project = choice->second;
            if (peerStorage.recipientRouteWouldCycle(id, environment, project))
                throw Conflict("This project would make project queues wait for one another. Choose another project or move the task.");
            task.projectId = project;
            taskRouting.rememberT3Project(bot, environment, project);
            next = TaskState::queued;
            database.writeJournal(id, "project_selected", encodeUserAnswer(entry));
        } else {
            auto updated = taskEvents.currentEvent(task);
            updated["userAnswer"] = answer;
            updated["userQuestion"] = wait->text;
            for (const auto* metadata : {"id", "failures", "retryAt", "evidence", "consumed"}) updated.erase(metadata);
            taskEvents.consumeEvent(task);
            taskEvents.event(task, "answer/" + std::to_string(waitId), updated);
            journalStorage.journal(id, entry);
        }
    } else if (operation == "approve" && reason == WaitReason::budget) {
        task.allowance.remaining += WaitStorage::initialWorkAllowance;
        database.writeJournal(id, "budget_granted", {{"waitId", waitId}, {"remaining", task.allowance.remaining}});
    } else if (operation == "approve" && reason == WaitReason::model_budget) {
        const Id eventId = task.eventId;
        const auto attempt = modelCalls.approveModelBudget(actor, modelCalls.taskModelBudget(id), "task_decision", 0, id, eventId);
        database.exec("UPDATE events SET reserved_attempt=? WHERE id=? AND task_id=?", {attempt, eventId, id});
        database.writeJournal(id, "model_budget_granted", {{"waitId", waitId}, {"attemptId", attempt}});
    } else if (operation == "retry" && (reason == WaitReason::model_failure || taskRouting.isUndispatchedNotesWait(task))) {
        if (reason == WaitReason::bot_notes_unavailable) next = TaskState::queued;
        if (const auto* failure = std::get_if<ModelFailureWait>(&wait->details);
            failure && failure->code == WaitFailureCode::unsupported_reasoning_effort)
            automationStorage.refreshPlannerModel(bot, task.config);
        database.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {task.eventId});
        task.feedback.reset();
        database.writeJournal(id, "model_retry", {{"waitId", waitId}});
    } else if (operation == "extend" && reason == WaitReason::deadline) {
        if (!data.contains("deadline") || !data["deadline"].is_number_integer() || data["deadline"].get<Seconds>() <= unixTime())
            throw Conflict("The new deadline must be in the future");
        const auto deadline = data.at("deadline").get<Seconds>();
        if (verification) {
            auto& run = destinationRun(task, verification->actionId);
            if (!run.outstanding()) throw RequestChanged{};
            run.deadline = deadline;
        } else {
            task.deadline = deadline;
        }
        next = TaskState::running;
        database.writeJournal(id, "deadline_extended", {{"deadline", deadline}, {"waitId", waitId}});
    } else if (operation == "reconcile" &&
               (reason == WaitReason::unknown_delivery || reason == WaitReason::auth || reason == WaitReason::settings)) {
        if (data.contains("session")) {
            const auto action = outboxStorage.outbox(task);
            if (reason != WaitReason::settings || (action && action->phase != OutboxPhase::ready))
                throw Conflict("Session settings can only be corrected before an unsent action");
            task.config["session"] = normalizeSessionSettings(data["session"]);
        }
        if (task.actionId.empty()) {
            if (task.activeRun)
                next = TaskState::running;
            else if (task.projectId.empty())
                next = TaskState::queued;
        } else {
            auto action = outboxStorage.outbox(task);
            if (!action) throw Conflict("No delivery to reconcile");
            auto& command = action->command;
            if (action->phase == OutboxPhase::ready && command.tool == CommandTool::answer_session_question) {
                const auto differs = [&](std::string_view key, const std::string& saved) {
                    const auto hint = field(data, key);
                    return !hint.empty() && hint != saved;
                };
                if (differs("threadId", command.target.threadId) || differs("runId", command.target.runId) ||
                    differs("questionId", command.questionId))
                    throw Conflict("The supplied T3 question is outside the saved answer destination");
            }
            RunRef hint{field(data, "threadId"), field(data, "runId"), {}, {}};
            if (!hint.threadId.empty() || !hint.runId.empty())
                command.candidate = std::move(hint);
            else
                command.candidate.reset();
            command.candidateQuestionId = field(data, "questionId");
            outboxStorage.saveOutbox(*action);
            next = TaskState::sending;
        }
        database.writeJournal(id, "reconcile_requested", {{"waitId", waitId}});
    } else {
        throw Conflict("Operation is unavailable for this waiting item");
    }
    waitStorage.clearWait(task, operation);
    if (operation == "answer" && task.question) task.question->target = questionTarget(task);
    task.state = next;
    taskRecords.save(task);
    if (kind) inboxStorage.recordInboxDecision(actor, {bot, *kind, waitId, waitId, id, revision, generation}, operation);
}

namespace {
Json journalRow(const Statement& row) {
    return {{"id", row.number(0)}, {"taskId", row.number(1)}, {"time", row.number(2)}, {"kind", row.string(3)}, {"data", row.json(4)}};
}
}
Json TaskHistory::history(Id taskId, Id after, unsigned limit) const {
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,
  CASE WHEN input.message_id IS NULL THEN j.data
       ELSE json_patch(j.data,json_object('status',input.status)) END
FROM journal j LEFT JOIN conversation_input_status input ON input.message_id=j.id
WHERE j.task_id=? AND j.id>? ORDER BY j.id LIMIT ?
)SQL",
                                {taskId, after, std::clamp(limit, 1u, 200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    return result;
}

Json Conversations::conversation(std::string_view bot, Id before, unsigned limit, ConversationEvents events,
                                 std::string_view viewer) const {
    if (before == std::numeric_limits<Id>::min()) return Json::array();
    const auto through = before ? before - 1 : std::numeric_limits<Id>::max();
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query(R"SQL(
WITH visible AS NOT MATERIALIZED (
  SELECT id,task_id,conversation_bot_id FROM journal
  WHERE id<=?2
    AND (?3=0 OR kind IN ('conversation_reply','report','completed','pr_review_requested')
         OR (kind='waiting' AND wait_reason IN ('user_question','model_failure')))
    AND ((conversation_bot_id IS NOT NULL AND kind NOT IN ('git_webhook','conversation_budget_approved','session_participation_stop'))
      OR (conversation_bot_id IS NULL
        AND kind IN ('request','waiting','wait_closed','user_answer','report','completed','cancelled','bot_message','pr_review_requested')
        AND (kind!='request' OR json_extract(data,'$.source') IS NOT 'conversation')
        AND (kind!='user_answer' OR user_answer_conversation IS NOT 1)))
), bot_page AS (
  SELECT id FROM visible WHERE conversation_bot_id=?1 ORDER BY id DESC LIMIT ?4
), page_ids AS MATERIALIZED (
  -- A row past the page limit within one task cannot reach the combined page.
  SELECT candidate.value AS id FROM tasks t
  CROSS JOIN json_each((SELECT json_group_array(id) FROM (
    SELECT id FROM visible WHERE task_id=t.id AND conversation_bot_id IS NULL
    ORDER BY id DESC LIMIT ?4
  ))) candidate
  WHERE t.bot_id=?1
  UNION ALL SELECT id FROM bot_page
  ORDER BY id DESC LIMIT ?4
), page AS (
  SELECT j.id,j.task_id,j.time,j.kind,j.wait_id,
    CASE WHEN j.kind='cancelled' AND t.id IS NOT NULL
         THEN json_set(j.data,'$.request',t.request) ELSE j.data END AS data
  FROM page_ids ids JOIN journal j ON j.id=ids.id
  LEFT JOIN tasks t ON j.kind='cancelled' AND j.conversation_bot_id IS NULL AND t.id=j.task_id
)
SELECT page.id,page.task_id,page.time,page.kind,
  CASE WHEN m.id IS NULL THEN page.data
  ELSE json_patch(page.data,json_object('messageId',m.id,'text',m.text,'sequence',m.sequence,
            'replyToMessageId',coalesce(m.reply_to,0),'status',coalesce(input.status,'received'),
            'imageAttachmentCount',(SELECT count(*) FROM message_attachments a
              WHERE a.message_id=m.id AND json_extract(a.metadata,'$.kind')='image'),
            'deliveredToT3',json(CASE WHEN (
              WITH instructions AS (
                SELECT kind,task_id,instruction_step AS step
                FROM journal WHERE kind IN ('instruction_accepted','instruction_delivered')
                AND message_id=m.id
              )
              SELECT min(EXISTS (
                SELECT 1 FROM instructions d WHERE d.kind='instruction_delivered'
                AND d.task_id=i.task_id AND d.step=i.step
              )) FROM instructions i WHERE i.kind!='instruction_delivered'
            ) THEN 'true' ELSE 'false' END))) END,
  channel.message_id IS NOT NULL,w.id,w.reason,w.data
FROM page LEFT JOIN messages m ON m.id=page.id
LEFT JOIN conversation_input_status input ON input.message_id=m.id
LEFT JOIN waits w ON page.kind='user_answer'
  AND w.id=page.wait_id AND w.task_id=page.task_id
LEFT JOIN channel_messages channel ON page.kind='conversation_message' AND channel.message_id=m.id
ORDER BY page.id DESC)SQL",
                                {bot, through, events == ConversationEvents::incoming ? 1 : 0, std::clamp(limit, 1u, 200u)});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (event.at("kind") == "memory_remembered") memory_.projectMemoryNotice(bot, event.at("data"));
        if (rows.number(5)) event["data"].update(channels_.channelContext(rows.number(0), bot, nullptr, viewer));
        if (rows.number(6)) event["data"]["question"] = decodeWaitRow(rows, 6).text;
        result.push_back(std::move(event));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

Json PeerConversations::peerConversation(std::string_view bot, std::string_view peer, Id before, unsigned limit, Id from,
                                         std::string_view viewer) const {
    std::lock_guard lock(database_.mutex);
    const auto sql = std::string(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,j.data,m.text,original.time
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message' AND j.exchange_id=p.id
LEFT JOIN messages m ON ?5<>'' AND m.id=j.forwarded_source_message_id
  AND m.id=sender.source_message_id AND m.bot_id=sender.bot_id AND m.person_id=?5
LEFT JOIN journal original ON original.id=m.id AND original.kind='conversation_message'
WHERE ((sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1))
)SQL") + (from ? " AND j.id>=?3 ORDER BY j.id ASC LIMIT ?4" : " AND (?3=0 OR j.id<?3) ORDER BY j.id DESC LIMIT ?4");
    auto rows = database_.query(sql.c_str(), {bot, peer, from ? from : before, std::clamp(limit, 1u, 200u), viewer});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (!rows.null(6)) {
            const auto forwarded = decodeBotMessage(rows.number(0), event.at("data")).forwarded;
            if (forwarded && forwarded->text == rows.string(5) && forwarded->time == rows.number(6)) event["forwardedFromViewer"] = true;
        }
        result.push_back(std::move(event));
    }
    if (!from) std::reverse(result.begin(), result.end());
    return result;
}

Id PeerConversations::peerMessageAnchor(std::string_view bot, std::string_view peer, Id event) const {
    std::lock_guard lock(database_.mutex);
    // Each participant has a journal row. The shared DM displays the sender task's copy.
    auto row = database_.query(R"SQL(
SELECT j.id
FROM journal source
JOIN peer_exchanges p ON p.id=source.exchange_id
JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message'
  AND j.exchange_id=p.id AND j.peer_message_id=source.peer_message_id
WHERE source.id=?3 AND source.kind='bot_message' AND source.task_id IN (p.sender_task,p.recipient_task)
  AND ((sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1))
LIMIT 1
)SQL",
                               {bot, peer, event});
    return row.next() ? row.number(0) : 0;
}

std::vector<std::pair<std::string, std::string>> PeerConversations::peerExchangePairs() const {
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query(R"SQL(
SELECT DISTINCT sender.bot_id,recipient.bot_id
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
)SQL");
    std::vector<std::pair<std::string, std::string>> result;
    while (rows.next()) result.emplace_back(rows.string(0), rows.string(1));
    return result;
}

bool PeerConversations::hasPeerExchange(std::string_view bot, std::string_view peer) const {
    std::lock_guard lock(database_.mutex);
    return database_
        .query(R"SQL(
SELECT 1
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE (sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1)
LIMIT 1
)SQL",
               {bot, peer})
        .next();
}
} // namespace flockv0
