#include "flock/external_mcp.hpp"
#include "flock/limits.hpp"
#include "connector_credentials.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr std::string_view tokenPrefix = "flock-mcp-";
constexpr std::string_view botUnavailable = "The Bot is unavailable to this connection";
constexpr std::string_view taskUnavailable = "The task is unavailable to this connection";

std::string redactConnectionTokens(std::string text) {
    return connector_credentials::redact(redactT3Tokens(std::move(text)), tokenPrefix, "[External MCP token]");
}

Json currentRun(const TaskData& task) {
    if (task.state == TaskState::cancelled || task.replanning || task.newSessionRequired) return nullptr;
    const auto& run = task.activeRun ? task.activeRun : task.lastRun;
    if (!run || run->environmentId != task.environmentId || run->assignment.revisionId != task.currentRevision) return nullptr;
    return assignedRunJson(run);
}
}

ExternalMcpCredential ExternalMcp::connect(std::string_view person, const std::vector<std::string>& bots) {
    if (bots.empty()) throw Conflict("At least one Bot is required");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto actor = db.sessionActor(person);
    if (actor.id.empty()) throw Conflict("The connected person is unavailable");
    db.secretStorage.sharedRedactor().requirePublic({person, bots});
    ExternalMcpCredential credential{actionId(), connector_credentials::bearer(tokenPrefix)};
    db.exec("INSERT INTO external_mcp_connections VALUES(?,?,?)",
            {credential.connectionId, actor.id, connector_credentials::digest(credential.bearerToken)});
    for (const auto& bot : bots) {
        if (!store_.policy().allows(actor, Action::bot_decide, bot)) throw Conflict(std::string(botUnavailable));
        db.exec("INSERT OR IGNORE INTO external_mcp_allowed_bots VALUES(?,?)", {credential.connectionId, bot});
    }
    tx.commit();
    return credential;
}

void ExternalMcp::disconnect(std::string_view connectionId) {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    db.exec("UPDATE external_mcp_connections SET token_digest=NULL WHERE id=? AND token_digest IS NOT NULL", {connectionId});
    if (!sqlite3_changes(db.db)) throw Conflict("No active external MCP connection with this ID");
}

std::optional<ExternalMcpConnection> ExternalMcp::authenticate(std::string_view token) const {
    if (!connector_credentials::validBearer(token, tokenPrefix)) return std::nullopt;
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto row = db.query("SELECT id,person_id FROM external_mcp_connections WHERE token_digest=?", {connector_credentials::digest(token)});
    if (!row.next() || db.sessionActor(row.string(1)).id.empty()) return std::nullopt;
    return ExternalMcpConnection{row.string(0)};
}

Id ExternalMcp::submit(const ExternalMcpConnection& connection, std::string_view bot, std::string_view requestId, std::string_view text,
                       const IntakeResolver& intake) {
    if (!validBotId(bot) || requestId.empty() || requestId.size() > 128 ||
        requestId.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") != requestId.npos ||
        redactConnectionTokens(std::string(requestId)) != requestId || text.size() > limits::mcp::maxRequestTextBytes ||
        text.find('\0') != text.npos || text.find_first_not_of(" \t\r\n") == text.npos)
        throw Conflict("Invalid task request");

    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    auto grant = db.query(R"SQL(
SELECT c.person_id FROM external_mcp_connections c JOIN external_mcp_allowed_bots b ON b.connection_id=c.id
WHERE c.id=? AND c.token_digest IS NOT NULL AND b.bot_id=?
)SQL",
                          {connection.id, bot});
    if (!grant.next()) throw Conflict(std::string(botUnavailable));
    const auto actor = db.sessionActor(grant.string(0));
    if (!store_.policy().allows(actor, Action::bot_decide, bot)) throw Conflict(std::string(botUnavailable));

    const auto fingerprint = connector_credentials::digest(Json::array({bot, text}).dump());
    auto receipt = db.query("SELECT request_digest,task_id FROM external_mcp_receipts WHERE connection_id=? AND request_id=?",
                            {connection.id, requestId});
    if (receipt.next()) {
        if (receipt.string(0) != fingerprint) throw Conflict("This requestId already has different arguments");
        return receipt.number(1);
    }
    if (db.botPaused(bot)) throw BotPaused{};

    const auto scrub = db.secretStorage.sharedRedactor();
    scrub.requirePublic(requestId);
    const auto request = redactConnectionTokens(scrub.text(text));
    Intake item;
    try {
        item = intake(bot, request);
    } catch (const std::exception&) {
        throw Conflict(std::string(botUnavailable));
    }
    if (item.botId != bot) throw Conflict(std::string(botUnavailable));
    item.request = request;
    item.environmentId = db.defaultT3Environment(bot, item.environmentId, {});
    if (item.environmentId.empty()) throw Conflict("Connect T3 or configure a T3 environment for this Bot before submitting tasks");
    const auto task = db.submitRequest(item, &actor);
    db.exec("INSERT INTO external_mcp_receipts VALUES(?,?,?,?)", {connection.id, requestId, fingerprint, task});
    tx.commit();
    return task;
}

ExternalMcp::OwnedTask ExternalMcp::authorizeTask(const ExternalMcpConnection& connection, Id taskId) const {
    auto& db = *store_.p_;
    auto row = db.query(R"SQL(
SELECT c.person_id,t.bot_id FROM external_mcp_receipts r
JOIN external_mcp_connections c ON c.id=r.connection_id
JOIN tasks t ON t.id=r.task_id
JOIN external_mcp_allowed_bots b ON b.connection_id=c.id AND b.bot_id=t.bot_id
WHERE r.connection_id=? AND r.task_id=? AND c.token_digest IS NOT NULL
)SQL",
                        {connection.id, taskId});
    if (!row.next()) throw Conflict(std::string(taskUnavailable));
    auto actor = db.sessionActor(row.string(0));
    auto bot = row.string(1);
    if (!store_.policy().allows(actor, Action::bot_decide, bot)) throw Conflict(std::string(taskUnavailable));
    return {std::move(actor), std::move(bot)};
}

Json ExternalMcp::completionRun(const OwnedTask& owner, const CompletionEntry& completion) const {
    if (const auto* source = completion.source()) return sourceRunJson(*source);
    const auto* task = std::get_if<TaskCompletion>(&completion.details);
    if (!task) return nullptr;
    const auto* dependency = std::get_if<PeerCompletion>(&task->provenance);
    if (!dependency) return nullptr;

    const auto& peerBot = dependency->botId;
    const auto& message = dependency->messageId;
    const auto policy = store_.policy();
    if (message.empty() || !policy.allows(owner.actor, Action::bot_view, peerBot)) return nullptr;
    const auto source = std::ranges::find_if(dependency->evidence, [&](const CompletionEvidence& ref) {
        return ref.kind == CompletionEvidenceKind::peer_report && ref.messageId == message;
    });
    if (source == dependency->evidence.end()) return nullptr;

    const auto& environment = source->environmentId;
    const auto& thread = source->threadId;
    const auto& resultRunId = source->runId;
    if (environment.empty() || thread.empty() || resultRunId.empty() || !policy.allows(peerBot, Resource::sessionRead(environment, thread)))
        return nullptr;

    auto row = store_.p_->query(R"SQL(
SELECT json_extract(j.data,'$.run.link') FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE j.task_id=? AND t.bot_id=? AND j.kind='report' AND j.run_environment_id=?
  AND j.run_thread_id=? AND j.result_run_id=?
ORDER BY j.id DESC LIMIT 1
)SQL",
                                {dependency->taskId, peerBot, environment, thread, resultRunId});
    if (!row.next()) return nullptr;
    return {{"environmentId", environment}, {"threadId", thread}, {"link", row.string(0)}};
}

Json ExternalMcp::status(const ExternalMcpConnection& connection, Id taskId) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db, Transaction::Mode::read);
    const auto owner = authorizeTask(connection, taskId);
    const auto task = db.taskRecords.load(taskId);
    const auto scrub = db.secretStorage.redactor(owner.bot);
    const auto publicText = [&](std::string_view text) { return redactConnectionTokens(scrub.text(text)); };
    Json data{{"taskId", taskId},    {"bot", owner.bot},  {"status", taskStateName(task.state)},
              {"question", nullptr}, {"result", nullptr}, {"sessionRun", nullptr}};
    if (const auto wait = db.waitStorage.currentWait(taskId)) {
        data["waitReason"] = waitReasonName(wait->reason());
        if (task.state == TaskState::waiting && wait->reason() == WaitReason::user_question && db.waitStorage.waitIsActionable(task)) {
            auto choices = Json::array();
            for (const auto& choice : wait->choices()) choices.push_back(publicText(choice));
            const auto target = TaskTarget::from(task, wait->id);
            data["question"] = {{"waitId", target.waitId},
                                {"targetRevision", target.revision},
                                {"targetGeneration", target.generation},
                                {"text", publicText(wait->text)},
                                {"choices", std::move(choices)}};
        }
    }
    Json run;
    if (task.state == TaskState::completed) {
        auto row = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='completed'"
                            " ORDER BY id DESC LIMIT 1",
                            {taskId});
        if (row.next()) {
            const auto completion = decodeCompletion(row.number(0), row.json(1));
            data["result"] = {{"text", publicText(completion.text)}};
            run = completionRun(owner, completion);
        }
    } else
        run = currentRun(task);

    const auto environment = field(run, "environmentId");
    const auto thread = field(run, "threadId");
    if (!environment.empty() && !thread.empty() && store_.policy().allows(owner.bot, Resource::sessionRead(environment, thread))) {
        Json session{{"environmentId", environment}, {"threadId", thread}, {"link", field(run, "link")}};
        const auto encoded = session.dump();
        if (scrub.record(session, SecretRedactor::Fields::external) == session && redactConnectionTokens(encoded) == encoded)
            data["sessionRun"] = std::move(session);
    }
    tx.commit();
    return data;
}

void ExternalMcp::answer(const ExternalMcpConnection& connection, Id taskId, Id waitId, Id revision, Id generation, std::string_view text) {
    if (text.find('\0') != text.npos) throw Conflict("Invalid task answer");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto owner = authorizeTask(connection, taskId);
    db.operate(owner.actor, {owner.bot, taskId, waitId, revision, generation}, "answer",
               {{"text", redactConnectionTokens(std::string(text))}}, 0, InboxKind::user_question);
    tx.commit();
}
}
