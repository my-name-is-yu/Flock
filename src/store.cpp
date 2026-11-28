#include "flock/bot_lifecycle.hpp"
#include "flock/digest.hpp"
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
#include "flock/channels.hpp"
#include "flock/local_machines.hpp"
#include "flock/identity.hpp"
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
#include "bot_lifecycle_storage.hpp"
#include "automation_storage.hpp"
#include "session_stop.hpp"
#include "wait_storage.hpp"
#include "task_events.hpp"
#include "journal_storage.hpp"
#include "store_access.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "task_storage.hpp"
#include "task_interaction_storage.hpp"
#include "task_context_storage.hpp"
#include "outbox_storage.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/store_options.hpp"
#include "flock/json.hpp"
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
#include <array>
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
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = random() & 0xff;
    return "flock-" + encodeHex(bytes);
}
Json runJson(const RunRef& run) {
    Json result{{"threadId", run.threadId}, {"runId", run.runId}, {"link", run.link}};
    if (!run.gatewayEnvironmentId.empty()) result["gatewayEnvironmentId"] = run.gatewayEnvironmentId;
    return result;
}
RunRef runRef(const Json& data) {
    return {field(data, "threadId"), field(data, "runId"), field(data, "link"), field(data, "gatewayEnvironmentId")};
}

Store::Impl::Impl(Store& store, std::filesystem::path file, StoreOptions options)
    : store(store), options(std::move(options)), path(std::move(file)) {
    database.open(path);
}

void TaskOperations::requestCancellation(TaskData& task, SessionStop stop) {
    const Id id = task.id;
    const bool undispatched = task.state == TaskState::queued || taskRouting.isUndispatchedNotesWait(task);
    if (!task.pendingInstructions.empty()) task.cancelledInstructions = task.pendingInstructions;
    waitStorage.clearWait(task, "cancel");
    taskEvents.consumeEvent(task);
    journalStorage.journalCancelRequested(id, stop);
    if (undispatched && task.actionId.empty()) {
        task.pendingDestination.reset();
        task.replanning = false;
        task.pendingInstructions.clear();
        task.state = TaskState::cancelled;
        journalStorage.journalCancelled(id);
    } else
        task.state = TaskState::cancelling;
    taskRecords.save(task);
}

Store::Store(std::filesystem::path db) : Store(std::move(db), {}) {}
Store::Store(std::filesystem::path db, StoreOptions options) : p_(std::make_unique<Impl>(*this, std::move(db), std::move(options))) {}
Store::~Store() = default;

void GitEvents::protectGitCredential(std::string value) {
    secrets_.protectGitCredential(std::move(value));
}
Database& Database::of(Store& store) {
    return StoreAccess::get(store).database;
}
Tasks::Tasks(Store& store) : database_(Database::of(store)), records_(StoreAccess::get(store).taskRecords) {}

TaskList::TaskList(Store& store)
    : database_(Database::of(store)), records_(StoreAccess::get(store).taskRecords), waits_(StoreAccess::get(store).waitStorage) {}

bool Tasks::hasTask(Id id) const {
    std::lock_guard lock(database_.mutex);
    return database_.query("SELECT 1 FROM tasks WHERE id=?", {id}).next();
}

TaskView Tasks::task(Id id) const {
    std::lock_guard lock(database_.mutex);
    return records_.taskDetails(id, true);
}

ConversationIntake::ConversationIntake(Store& store) : turns_(StoreAccess::get(store).conversationTurns) {}

WebConversationIntake::WebConversationIntake(Store& store)
    : database_(Database::of(store)), turns_(StoreAccess::get(store).conversationTurns), permissions_(StoreAccess::get(store).permissions) {
}

Json ConversationIntake::receive(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                                 const Json& attachments) {
    return turns_.receive(
        actor, routing.botId, clientId, text, replyTo, conversation::PrincipalSource::trusted, nullptr, [&] { return routing; },
        attachments);
}

Conversations::Conversations(Store& store)
    : database_(Database::of(store)), context_(StoreAccess::get(store).conversationContextStorage),
      memory_(StoreAccess::get(store).memoryStorage), channels_(StoreAccess::get(store).channelStorage) {}

Memory::Memory(Store& store) : database_(Database::of(store)), memory_(StoreAccess::get(store).memoryStorage) {}

Automations::Automations(Store& store)
    : database_(Database::of(store)), automation_(StoreAccess::get(store).automationStorage),
      lifecycle_(StoreAccess::get(store).botLifecycle), secrets_(StoreAccess::get(store).secretStorage),
      tasks_(StoreAccess::get(store).taskOperations), policy_(StoreAccess::get(store).permissions) {}

Inbox::Inbox(Store& store) : inbox_(StoreAccess::get(store).inboxStorage) {}

GitEvents::GitEvents(Store& store) : git_(StoreAccess::get(store).gitStorage), secrets_(StoreAccess::get(store).secretStorage) {}

TaskIntake::TaskIntake(Store& store) : database_(Database::of(store)), operations_(StoreAccess::get(store).taskOperations) {}

TaskControl::TaskControl(Store& store) : database_(Database::of(store)), operations_(StoreAccess::get(store).taskOperations) {}

TaskHistory::TaskHistory(Store& store) : database_(Database::of(store)), records_(StoreAccess::get(store).taskRecords) {}

Channels::Channels(Store& store)
    : database_(Database::of(store)), channels_(StoreAccess::get(store).channelStorage), turns_(StoreAccess::get(store).conversationTurns) {
}

LocalMachines::LocalMachines(Store& store) : machines_(StoreAccess::get(store).localMachineStorage) {}

WebIdentity::WebIdentity(Store& store)
    : database_(Database::of(store)), machines_(StoreAccess::get(store).localMachineStorage),
      identity_(StoreAccess::get(store).webIdentity) {}

Notifications::Notifications(Store& store)
    : database_(Database::of(store)), secrets_(StoreAccess::get(store).secretStorage), channels_(StoreAccess::get(store).channelStorage),
      permissions_(StoreAccess::get(store).permissions) {}

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
SELECT DISTINCT m.bot_id,m.id FROM journal j JOIN messages m ON m.id=CASE j.kind
  WHEN 'conversation_reply' THEN j.reply_to_message_id ELSE j.message_id END
WHERE j.id>?1 AND j.id<=?2 AND (?3='' OR m.bot_id=?3)
  AND j.kind IN ('conversation_reply','instruction_accepted','instruction_delivered')
UNION ALL
SELECT conversation_bot_id,json_extract(data,'$.eventId') FROM journal
WHERE kind='bot_setup_undone' AND id>?1 AND id<=?2 AND (?3='' OR conversation_bot_id=?3)
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
WHERE j.kind='conversation_reply'
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
    if (!permissions.allows(*requester, Action::bot_talk, item.botId)) throw AccessDenied{};
    secretStorage.sharedRedactor().requirePublic(requester->id);
    return submit(item, 0, nullptr, {}, requester->id);
}

Id TaskOperations::submit(const Intake& item, Id sourceMessageId, const OutboxSource* source, std::optional<TaskTrigger> trigger,
                          std::optional<std::string> requester) {
    if (item.botId.empty() || item.request.find_first_not_of(" \r\n\t") == std::string::npos ||
        item.request.size() > limits::messages::maxBodyBytes || item.environmentId.empty() || !item.config.is_object())
        throw Conflict("Bot, nonempty request (up to " + std::to_string(limits::messages::maxBodyBytes) +
                       " bytes), and environment are required");
    if (botLifecycle.botDeleted(item.botId)) throw Conflict("Bot has been deleted");
    if (botLifecycle.botPaused(item.botId)) throw BotPaused{};
    const auto scrub = secretStorage.redactor(item.botId);
    scrub.requirePublic({item.botId, item.environmentId, item.projectId, item.config});
    const auto request = scrub.text(item.request);
    TaskData task{.state = TaskState::queued,
                  .botId = item.botId,
                  .request = request,
                  .intent = request,
                  .environmentId = item.environmentId,
                  .projectId = item.projectId,
                  .doneWhen = {},
                  .createdAt = unixTime(),
                  .config = item.config};
    if (sourceMessageId) task.sourceMessageId = sourceMessageId;
    if (source) {
        task.sourceTaskId = source->taskId;
        task.sourceResultRefs.push_back(*source);
    }
    task.trigger = std::move(trigger);
    task.requester = std::move(requester);
    const auto id = taskRecords.insertTask(task);
    Json entry = {{"text", request},
                  {"messageId", sourceMessageId},
                  {"source", sourceMessageId ? "conversation"
                             : task.trigger  ? "automation"
                                             : "manual"}};
    if (task.trigger) entry["trigger"] = taskTriggerJson(*task.trigger);
    database.writeJournal(id, JournalKind::request, entry);
    return id;
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
    if (!permissions.allows(actor, Action::bot_decide, bot)) throw AccessDenied{};
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
    if (task.generation != generation) throw RequestChanged{};
    const auto kind = inboxWaitKind(reason);
    if (expectedKind && kind != expectedKind) throw RequestChanged{};
    if (operation == "answer" && reason == WaitReason::user_question) {
        const auto owner = task.question ? task.question->target : waitQuestionTarget(*wait);
        if (owner != questionTarget(task, waitId)) throw RequestChanged{};
        const auto answer = field(data, "text");
        if (answer.find_first_not_of(" \r\n\t") == std::string::npos || answer.size() > limits::messages::maxWaitAnswerBytes)
            throw Conflict("A nonempty answer (up to " + std::to_string(limits::messages::maxWaitAnswerBytes) + " bytes) is required");
        const UserAnswerEntry entry{answer, waitId, sourceMessage ? std::optional{sourceMessage} : std::nullopt};
        auto updated = taskEvents.currentEvent(task);
        updated["userAnswer"] = answer;
        updated["userQuestion"] = wait->text;
        for (const auto* metadata : {"id", "failures", "retryAt", "evidence", "consumed"}) updated.erase(metadata);
        const auto answerId = journalStorage.journal(id, entry);
        if (field(updated, "kind") == "policy_outcome") updated["policyReply"] = {{"anchor", answerId}, {"personId", actor.id}};
        taskEvents.consumeEvent(task);
        taskEvents.event(task, "answer/" + std::to_string(waitId), updated);
    } else {
        throw Conflict("Operation is unavailable for this waiting item");
    }
    waitStorage.clearWait(task, operation);
    if (operation == "answer" && task.question) task.question->target = questionTarget(task);
    task.state = TaskState::deciding;
    taskRecords.save(task);
    if (kind) inboxDecisions.record(actor, {bot, *kind, waitId, waitId, id, revision, generation}, operation);
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

Json Conversations::conversation(std::string_view bot, Id before, unsigned limit, ConversationEvents events, const Person* viewer) const {
    if (before == std::numeric_limits<Id>::min()) return Json::array();
    const auto through = before ? before - 1 : std::numeric_limits<Id>::max();
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query(R"SQL(
WITH visible AS NOT MATERIALIZED (
  SELECT id,task_id,conversation_bot_id FROM journal
  WHERE id<=?2
    AND (?3=0 OR kind IN ('conversation_reply','report','completed')
         OR (kind='waiting' AND wait_reason IN ('user_question','bot_notes_unavailable','auth','settings')))
    AND ((conversation_bot_id IS NOT NULL AND kind NOT IN ('git_webhook','session_participation_stop','bot_setup_undone'))
      OR (conversation_bot_id IS NULL
        AND kind IN ('request','waiting','wait_closed','user_answer','report','completed','cancelled')
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
        if (event.at("kind") == "memory_remembered" || event.at("kind") == "memory_learned")
            memory_.projectMemoryNotice(bot, event.at("data"), viewer);
        if (rows.number(5))
            event["data"].update(
                channels_.channelContext(rows.number(0), bot, nullptr, viewer ? std::string_view(viewer->id) : std::string_view{}));
        if (rows.number(6)) event["data"]["question"] = decodeWaitRow(rows, 6).text;
        result.push_back(std::move(event));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace flockv0
