#include "policy_internal.hpp"
#include "conversation_turns.hpp"
#include "flock/decision_errors.hpp"
#include "outbox_storage.hpp"
#include "conversation_storage.hpp"
#include "web_identity_state.hpp"
#include "wait_storage.hpp"
#include "task_records.hpp"
#include "secret_storage.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "flock/board_inbox.hpp"
#include "database.hpp"
#include "policy_approvals.hpp"
#include "bot_setup_storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "policy_origin.hpp"
#include "bot_lifecycle_storage.hpp"
#include "channel_storage.hpp"
#include "inbox_storage.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
using conversation_storage::integer;

std::string fingerprint(const Json& payload) {
    const auto encoded = payload.dump();
    unsigned char bytes[32];
    unsigned count = 0;
    if (EVP_Digest(encoded.data(), encoded.size(), bytes, &count, EVP_sha256(), nullptr) != 1 || count != sizeof(bytes))
        throw Conflict("Cannot bind this action to an approval");
    std::string hex;
    for (const auto byte : bytes) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

Person currentApprovalActor(const Policy& policy, const WebIdentityResolver& resolve, bool liveInteractiveRoles, const Person& actor) {
    // Managed accounts can be edited after the HTTP request authenticates.
    // Host-managed credentials may have different roles for the same person.
    if (!liveInteractiveRoles || !resolve || !policy.requiresIdentity()) return actor;
    const auto role = resolve(actor.id);
    return role ? Person{actor.id, actor.adminRoot && *role} : Person{};
}
}

std::string_view approvalError(ApprovalGate result) {
    if (result == ApprovalGate::waiting) return "The action is waiting for approval in Flock.";
    if (result == ApprovalGate::expired) return "The policy approval expired.";
    if (result == ApprovalGate::stale) return "The action changed or its policy approval is no longer current.";
    return "The action was denied by policy.";
}

PolicyOrigin PolicyApprovals::policyOrigin(const conversation::Message& message) {
    const bool background = message.routing.contains("gitEvent");
    PolicyOrigin origin{message.bot, message.channelScope, {}, message.id, message.id, 0, 0, 0, background};
    if (background) return origin;
    if (message.principalSource != conversation::PrincipalSource::channel) {
        origin.requester = conversationTurns.turnActor(message).id;
    } else if (channelStorage.channelMessageAuthorized(message.id, message.bot)) {
        const auto& id = message.admittedActor.id;
        const bool canSignIn =
            webIdentity.resolve ? webIdentity.resolve(id).has_value() : id == localOwner.id && !permissions.requiresIdentity();
        if (canSignIn) origin.requester = id;
    }
    return origin;
}

PolicyOrigin PolicyApprovals::taskPolicyOrigin(const TaskData& task) {
    const auto id = task.id;
    auto source = task.criteriaProposal ? task.criteriaProposal->sourceMessageId.value_or(0) : Id{0};
    if (!source) source = task.sourceMessageId.value_or(0);
    for (const auto& instruction : task.pendingInstructions) source = std::max(source, instruction.messageId);
    PolicyOrigin origin{task.botId, {}, {}, source, 0, id, task.currentRevision.value_or(0), task.generation, !source};
    if (!source) origin.scope = "task/" + std::to_string(id);
    if (source) {
        const auto message = conversationTurns.loadTurnMessage(source);
        const auto fromMessage = policyOrigin(message);
        origin.scope = fromMessage.scope;
        origin.requester = fromMessage.requester;
        origin.background = fromMessage.background;
        if (message.bot != origin.bot && !origin.requester.empty()) origin.scope = "task/" + std::to_string(id);
    } else if (const auto& requester = task.requester; requester && !requester->empty()) {
        origin.requester = *requester;
        origin.background = false;
    } else if (task.trigger) {
        auto owner = database.query("SELECT person FROM bot_owners WHERE bot=?", {origin.bot});
        origin.requester = owner.next() ? owner.string(0) : localOwner.id;
    } else {
        auto exchange = database.query("SELECT sender_task FROM peer_exchanges WHERE recipient_task=? AND sender_task<?", {id, id});
        if (exchange.next()) {
            const auto parent = taskPolicyOrigin(taskRecords.load(exchange.number(0)));
            origin.requester = parent.requester;
            origin.background = parent.background;
            origin.anchor = parent.anchor;
            origin.scope = parent.scope;
        }
    }
    return origin;
}

bool PolicyApprovals::currentPolicyApproval(Id turn, Id task, Id revision, Id generation, std::string_view operation) const {
    if (turn &&
        !database.query("SELECT 1 FROM conversation_turns WHERE message_id=? AND status IN ('policy_waiting','running')", {turn}).next())
        return false;
    if (!task) return true;
    if (operation.starts_with(runOutboxOperation) || operation.starts_with(runStopOperation)) {
        const bool stopOperation = operation.starts_with(runStopOperation);
        const auto prefix = stopOperation ? runStopOperation : runOutboxOperation;
        if (stopOperation)
            return database
                .query(R"SQL(
SELECT 1 FROM outbox o JOIN tasks t ON t.id=o.task_id
WHERE t.id=? AND (o.id=? OR o.stop_operation=?)
  AND o.phase IN ('started','accepted') AND o.stop_operation IS NOT NULL
  AND t.state IN ('running','waiting')
  AND coalesce(t.current_revision,0)=?
)SQL",
                       {task, operation.substr(prefix.size()), operation.substr(prefix.size()), revision})
                .next();
        return database
            .query(R"SQL(
SELECT 1 FROM outbox o JOIN tasks t ON t.id=o.task_id
WHERE o.id=? AND t.id=? AND o.phase='ready'
  AND t.state IN ('running','waiting')
  AND coalesce(t.current_revision,0)=?
)SQL",
                   {operation.substr(prefix.size()), task, revision})
            .next();
    }
    return database
        .query("SELECT 1 FROM tasks WHERE id=? AND generation=?"
               " AND coalesce(current_revision,0)=?"
               " AND state NOT IN ('cancelling','cancelled') AND (? OR state<>'completed')",
               {task, generation, revision, turn ? 1 : 0})
        .next();
}

void PolicyApprovals::releasePolicyTurn(Id turn) {
    if (turn)
        database.exec("UPDATE conversation_turns SET status='running' WHERE message_id=? AND status='policy_waiting'"
                      " AND NOT EXISTS (SELECT 1 FROM policy_approvals WHERE turn=? AND status='pending')",
                      {turn, turn});
}

void PolicyApprovals::releasePolicyTask(Id approval, std::string_view resolution) {
    auto row = database.query("SELECT task,wait_id,resume_state,operation FROM policy_approvals WHERE id=? AND wait_id<>0", {approval});
    if (!row.next()) return;
    auto task = taskRecords.load(row.number(0));
    const auto wait = waitStorage.currentWait(row.number(0));
    if (task.state != TaskState::waiting || !wait || wait->id != row.number(1)) return;
    waitStorage.clearWait(task, resolution);
    task.state = decodeTaskState(task.id, row.string(2));
    if (row.string(3).starts_with(taskSetupOperation) && task.pendingDecision) task.pendingDecision->generation = task.generation + 1;
    taskRecords.save(task);
    database.exec("UPDATE policy_approvals SET generation=? WHERE id=?", {task.generation, approval});
}

void PolicyApprovals::settlePolicyApprovals(std::optional<Seconds> now) {
    auto rows = database.query(R"SQL(
SELECT a.id,a.turn,a.task,a.revision,a.generation,a.expires_at,a.mode_revision=s.revision,a.status,a.operation
FROM policy_approvals a CROSS JOIN policy_settings s
WHERE a.status IN ('pending','approved')
)SQL");
    struct Settlement {
        Id id, turn, task;
        std::string_view status;
    };
    std::vector<Settlement> settled;
    while (rows.next()) {
        const bool stale =
            !rows.number(6) || !currentPolicyApproval(rows.number(1), rows.number(2), rows.number(3), rows.number(4), rows.string(8));
        const bool expired = rows.string(7) == "pending" && now && rows.number(5) && rows.number(5) <= *now;
        if (stale || expired) settled.push_back({rows.number(0), rows.number(1), rows.number(2), stale ? "stale" : "expired"});
    }
    for (const auto& change : settled) {
        database.exec("UPDATE policy_approvals SET status=? WHERE id=? AND status IN ('pending','approved')", {change.status, change.id});
        releasePolicyTurn(change.turn);
        releasePolicyTask(change.id, change.status);
        policy_storage::audit(database, Person{}, {}, "approval_closed", {{"itemId", change.id}, {"status", change.status}});
        if (change.task) database.writeJournal(change.task, "policy_approval", {{"itemId", change.id}, {"status", change.status}});
    }
}

void PolicyApprovals::consumePolicyApproval(std::string_view operation) {
    database.exec("UPDATE policy_approvals SET status='used' WHERE operation=? AND status='approved'", {operation});
}

ApprovalGate PolicyApprovals::approveAction(const PolicyOrigin& origin, const Resource& resource, std::string_view operation,
                                            const Json& payload, Seconds now, bool claim, ApprovalWait waitMode) {
    Policy policy = permissions;
    const auto action = actionName(resource.action);
    const auto deny = [&](std::string_view reason) {
        auto details = secretStorage.sharedRedactor().record({{"action", action}, {"target", resource.target}, {"reason", reason}});
        policy_storage::audit(database, Person{}, origin.bot, "action_denied", details);
        if (origin.task) database.writeJournal(origin.task, "policy_denied", details);
        return ApprovalGate::denied;
    };
    if (botLifecycle.botPaused(origin.bot)) return ApprovalGate::stale;
    if (policy.forbidden(origin.bot, resource.action, resource.target)) return deny("never");
    auto row = database.query(R"SQL(
SELECT a.id,a.action,a.target,a.fingerprint,a.status,a.mode_revision=s.revision,a.expires_at,
       a.turn,a.task,a.revision,a.generation
FROM policy_approvals a CROSS JOIN policy_settings s WHERE a.operation=?
)SQL",
                              {operation});
    if (row.next()) {
        const auto status = row.string(4);
        if (row.string(1) != action || row.string(2) != resource.target || row.string(3) != fingerprint(payload) || !row.number(5) ||
            row.number(7) != origin.turn || row.number(8) != origin.task || row.number(9) != origin.revision ||
            (!operation.starts_with(runOutboxOperation) && !operation.starts_with(runStopOperation) &&
             row.number(10) != origin.generation) ||
            !currentPolicyApproval(origin.turn, origin.task, origin.revision, origin.generation, operation))
            return ApprovalGate::stale;
        if (status == "expired" || status == "rule_saved" || (status == "pending" && row.number(6) && row.number(6) <= now))
            return ApprovalGate::expired;
        if (status == "denied") return ApprovalGate::denied;
        if (status == "pending") return ApprovalGate::waiting;
        if (status != "approved") return ApprovalGate::stale;
        if (claim) database.exec("UPDATE policy_approvals SET status='used' WHERE id=? AND status='approved'", {row.number(0)});
        return ApprovalGate::allow;
    }
    if (database
            .query(R"SQL(
SELECT 1 FROM policy_approvals
WHERE bot=? AND scope=? AND action=? AND target=? AND status='denied'
AND (anchor>=? OR requester='' OR ?='')
)SQL",
                   {origin.bot, origin.scope, action, resource.target, origin.anchor, origin.requester})
            .next())
        return deny("previously_denied");
    const auto result = policy.check(origin.bot, resource);
    if (result == PolicyResult::allow) return ApprovalGate::allow;
    if (origin.requester.empty()) return deny("no_person_requester");
    if (secretStorage.sharedRedactor().text(origin.requester) != origin.requester) return deny("protected_requester");
    if (!currentPolicyApproval(origin.turn, origin.task, origin.revision, origin.generation, operation)) return ApprovalGate::stale;
    const auto scrub = secretStorage.sharedRedactor();
    if (scrub.text(resource.target) != resource.target) return deny("protected_target");
    auto preview = [&](auto&& self, Json value) -> Json {
        if (value.is_object()) {
            for (const auto key : {"contentBase64", "html", "images", "config", "secret", "value", "token", "arguments", "answers"})
                value.erase(key);
        }
        if (value.is_structured())
            for (auto& child : value) child = self(self, std::move(child));
        return value;
    };
    const auto shown =
        resource.action == Action::secret_use
            ? std::string{}
            : bounded(scrub
                          .payload(resource.action == Action::private_memory_write ? Json("Restore the Bot’s earlier memory.")
                                                                                   : preview(preview, payload),
                                   false, 480)
                          .dump(-1, ' ', false, Json::error_handler_t::replace),
                      1200);
    database.exec(R"SQL(
INSERT INTO policy_approvals(operation,bot,action,target,fingerprint,preview,scope,requester,anchor,turn,
 task,revision,generation,created_at,expires_at,mode_revision,card_generation)
SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,revision,? FROM policy_settings WHERE id=1
)SQL",
                  {operation, origin.bot, action, resource.target, fingerprint(payload), shown, origin.scope, origin.requester,
                   origin.anchor, origin.turn, origin.task, origin.revision, origin.generation, now,
                   origin.background ? Json(now + 30 * 60) : Json(nullptr), origin.generation});
    const auto card = database.last();
    if (origin.turn && waitMode == ApprovalWait::pause)
        database.exec("UPDATE conversation_turns SET status='policy_waiting' WHERE message_id=? AND status='running'", {origin.turn});
    if (origin.task) {
        if (!origin.turn &&
            (operation.starts_with("outbox/") || operation.starts_with("git/") || operation.starts_with(taskSetupOperation))) {
            auto task = taskRecords.load(origin.task);
            if (task.state != TaskState::waiting) {
                const auto resume = taskStateName(task.state);
                const auto waitId = waitStorage.wait(task, PolicyApprovalWait{card}, std::string(action) + " · " + resource.target);
                taskRecords.save(task);
                database.exec("UPDATE policy_approvals SET wait_id=?,resume_state=?,generation=?,card_generation=? WHERE id=?",
                              {waitId, resume, task.generation, task.generation, card});
            }
        }
        database.writeJournal(origin.task, "policy_approval", {{"itemId", card}, {"status", "pending"}});
    }
    policy_storage::audit(database, Person{}, origin.bot, "approval_requested",
                          {{"itemId", card}, {"action", action}, {"target", resource.target}});
    if (origin.anchor) channelStorage.channelApprovalNotice(origin.anchor);
    return ApprovalGate::waiting;
}

void PolicyApprovals::decidePolicyApproval(const Person& actor, const InboxTarget& item, std::string_view choice) {
    auto row = database.query(R"SQL(
SELECT a.requester,a.action,a.target,a.status,a.turn,a.task,a.revision,a.generation,a.mode_revision=s.revision,a.wait_id,a.card_generation,a.operation
FROM policy_approvals a CROSS JOIN policy_settings s WHERE a.id=? AND a.bot=?
)SQL",
                              {item.itemId, item.botId});
    if (!row.next()) throw DecisionUnavailable{};
    Policy policy = permissions;
    const auto currentActor = currentApprovalActor(policy, webIdentity.resolve, webIdentity.liveInteractiveRoles, actor);
    const bool canSaveRule = policy.allows(currentActor, Action::rules_manage, "installation");
    const bool nextTime = choice == "allow_next_time";
    const bool saveRule = nextTime || choice == "always_allow";
    if (!canSaveRule && (saveRule || currentActor.id.empty() || currentActor.id != row.string(0))) throw DecisionDenied{};
    const auto status = row.string(3);
    if (item.waitId != row.number(9) || item.taskId != row.number(5) || item.targetRevision != row.number(6) ||
        item.targetGeneration != row.number(10))
        throw RequestChanged{};
    if (nextTime ? status != "expired" : status != "pending") throw DecisionUnavailable{};
    if (!nextTime && (!row.number(8) || !currentPolicyApproval(row.number(4), row.number(5), row.number(6), row.number(7), row.string(11))))
        throw RequestChanged{};
    if (saveRule)
        policy_storage::writeRule(database, secretStorage, currentActor, RuleKind::allow, *parseAction(row.string(1)), row.string(2), true,
                                  true);
    database.exec("UPDATE policy_approvals SET status=? WHERE id=?", {nextTime           ? "rule_saved"
                                                                      : choice == "deny" ? "denied"
                                                                                         : "approved",
                                                                      item.itemId});
    database.exec(R"SQL(
UPDATE channel_deliveries SET next_attempt=0 WHERE state='pending'
AND 'channel-task-update/' || id=(SELECT operation FROM policy_approvals WHERE id=?)
)SQL",
                  {item.itemId});
    releasePolicyTurn(row.number(4));
    releasePolicyTask(item.itemId, choice);
    inboxStorage.recordInboxDecision(currentActor, item, choice);
    policy_storage::audit(database, currentActor, item.botId, "approval_decided", {{"itemId", item.itemId}, {"decision", choice}});
}

Json PolicyApprovals::policyApprovalCards(const Person& actor, std::string_view bot) {
    auto rows = database.query(R"SQL(
SELECT a.id,a.action,a.target,a.preview,a.requester,a.anchor,a.task,a.revision,a.generation,a.created_at,a.expires_at,
       a.status,a.scope,a.turn,a.wait_id,a.card_generation,a.operation,o.url
FROM policy_approvals a LEFT JOIN git_pr_operations o ON a.scope GLOB 'git-pr/*' AND o.operation_id=substr(a.scope,8)
WHERE a.bot=? AND a.status IN ('pending','expired') ORDER BY a.id DESC
)SQL",
                               {bot});
    const Policy policy = permissions;
    const auto currentActor = currentApprovalActor(policy, webIdentity.resolve, webIdentity.liveInteractiveRoles, actor);
    const bool canSaveRule = policy.allows(currentActor, Action::rules_manage, "installation");
    auto cards = Json::array();
    while (rows.next()) {
        const bool expired = rows.string(11) == "expired";
        if (!expired && !currentPolicyApproval(rows.number(13), rows.number(6), rows.number(7), rows.number(8), rows.string(16))) continue;
        const bool requester = !currentActor.id.empty() && currentActor.id == rows.string(4);
        const bool canDecide = canSaveRule || (!expired && requester);
        const auto label = rows.string(1) + " · " + rows.string(2);
        Json item{{"botId", bot},
                  {"itemKind", "policy_approval"},
                  {"itemId", rows.number(0)},
                  {"waitId", rows.number(14)},
                  {"taskId", rows.number(6)},
                  {"targetRevision", rows.number(7)},
                  {"targetGeneration", rows.number(15)},
                  {"botName", bot},
                  {"requestTitle", label},
                  {"action", rows.string(1)},
                  {"target", rows.string(2)},
                  {"prompt", rows.string(3)},
                  {"status", rows.string(11)},
                  {"canDecide", canDecide},
                  {"canSaveRule", canSaveRule},
                  {"createdAt", rows.number(9)},
                  {"expiresAt", rows.number(10)}};
        if (const auto pr = rows.string(17); !pr.empty()) item["displayTarget"] = pr;
        if (!canSaveRule && !requester && !policy.allows(currentActor, Action::bot_view, bot)) continue;
        if (rows.number(5)) {
            auto source = database.query("SELECT bot_id FROM messages m JOIN journal j ON j.id=m.id"
                                         " WHERE m.id=? AND j.kind='conversation_message'"
                                         " AND NOT EXISTS (SELECT 1 FROM channel_messages WHERE message_id=m.id)",
                                         {rows.number(5)});
            if (source.next() && policy.allows(currentActor, Action::bot_view, source.string(0))) {
                item["conversationEventId"] = rows.number(5);
                item["conversationBotId"] = source.string(0);
            }
        }
        cards.push_back(secretStorage.sharedRedactor().record(std::move(item)));
    }
    return cards;
}
}
