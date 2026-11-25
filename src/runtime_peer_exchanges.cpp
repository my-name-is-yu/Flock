#include "runtime_peer_exchanges.hpp"

#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"

#include <algorithm>

namespace flockv0 {
Runtime::PeerExchanges::PeerExchanges(Store::Impl& db, const Policy& policy, TaskUpdates& updates, RunReports& reports,
                                      SessionHistory& history, const PeerResolver& resolvePeers)
    : db(db), policy(policy), updates(updates), reports(reports), history(history), resolvePeers(resolvePeers) {}

std::optional<Runtime::PeerExchanges::PeerResult> Runtime::PeerExchanges::peerResult(const TaskData& task, const Json& event) const {
    if (event.at("kind") != "bot_result") return std::nullopt;
    auto exchange =
        db.query(R"SQL(
SELECT p.recipient_task, p.kind, recipient.environment_id
FROM peer_exchanges p JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE p.id=? AND p.sender_task=? AND p.phase='closed' AND p.last_message=?
  AND p.sender_environment=? AND p.sender_revision IS ?
)SQL",
                 {event.at("exchangeId"), task.id, event.at("messageId"), task.environmentId, revisionJson(task.currentRevision)});
    if (!exchange.next()) return std::nullopt;
    return PeerResult{exchange.number(0), exchange.string(1), exchange.string(2)};
}

std::vector<PeerBot> Runtime::PeerExchanges::peersFor(std::string_view bot) const {
    if (!resolvePeers) return {};
    return resolvePeers(bot);
}

PeerBot Runtime::PeerExchanges::allowedPeer(std::string_view sender, std::string_view botId) const {
    if (botId.empty() || sender == botId) throw Conflict("Choose an allowed peer Bot");
    const auto peers = peersFor(sender);
    const auto peer = std::find_if(peers.begin(), peers.end(), [&](const PeerBot& p) { return p.intake.botId == botId; });
    if (peer == peers.end()) throw Conflict("The destination Bot is unavailable or not allowed");
    return *peer;
}

BotMessageEntry Runtime::PeerExchanges::peerMessage(const PeerExchange& peer, const TaskData& from, const TaskData& to,
                                                    std::string_view type, std::string_view body, std::string_view messageId) {
    return {.exchangeId = peer.id,
            .messageId = std::string(messageId),
            .fromBotId = from.botId,
            .toBotId = to.botId,
            .messageType = std::string(type),
            .requestKind = peer.kind,
            .text = std::string(body),
            .replyTo = peer.lastMessage.empty() ? std::nullopt : std::optional{peer.lastMessage}};
}

void Runtime::PeerExchanges::transitionPeer(const PeerExchange& peer, TaskData& actor, TaskData& task, const BotMessageEntry& message,
                                            std::optional<Id> forwardedSource) {
    const auto& type = message.messageType;
    const bool closing = message.status.has_value();
    const auto kind = closing ? "bot_result" : "bot_" + type;
    const auto phase = closing ? "closed" : type == "question" ? "question" : "working";
    if (type == "request")
        db.exec("INSERT INTO peer_exchanges(id,kind,phase,last_message,sender_task,recipient_task,"
                "sender_environment,sender_revision) VALUES(?,?,?,?,?,?,?,?)",
                {peer.id, peer.kind, phase, message.messageId, peer.sender, peer.recipient, actor.environmentId,
                 revisionJson(actor.currentRevision)});
    else
        db.exec("UPDATE peer_exchanges SET phase=?,last_message=? WHERE id=?", {phase, message.messageId, peer.id});
    db.peerMessage(peer, message, forwardedSource);
    updates.consumeEvent(actor);
    if (!closing) actor.state = TaskState::peer_waiting;
    if (terminal(task) || task.state == TaskState::cancelling) return;
    if (closing) db.clearWait(task, kind);
    updates.consumeEvent(task);
    auto event = encodeBotMessage(message);
    if (event.contains("evidence")) {
        event["references"] = std::move(event["evidence"]);
        event.erase("evidence");
    }
    event["kind"] = kind;
    db.event(task, "bot/" + message.messageId, std::move(event));
    if (task.state != TaskState::queued) task.state = TaskState::deciding;
    updates.saveTask(task);
}

std::optional<ForwardedPeerMessage> Runtime::PeerExchanges::forwardableUserMessage(const TaskData& task) const {
    if (!task.sourceMessageId || !task.sourceMessageCoversFullRequest.value_or(false)) return std::nullopt;
    const auto sourceId = *task.sourceMessageId;
    auto row = db.query(R"SQL(
SELECT m.text,j.time FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND m.bot_id=? AND j.kind='conversation_message'
)SQL",
                        {sourceId, task.botId});
    if (!row.next()) return std::nullopt;
    return ForwardedPeerMessage{row.string(0), row.number(1)};
}

void Runtime::PeerExchanges::sendPeer(TaskData& sender, const Json& event, const TaskDecision& decision, PeerBot peer,
                                      const std::string& action) {
    if (db.botPaused(peer.intake.botId)) throw BotPaused{};
    if (db.peerExchange(sender.id)) throw Conflict("Finish the current Bot exchange first");
    const auto& args = decision.args;
    const auto note = field(args, "text");
    std::optional<ForwardedPeerMessage> forwarded;
    if (args.value("forwardUserMessage", false)) {
        forwarded = forwardableUserMessage(sender);
        if (!forwarded) throw Conflict("This task has no complete user message to forward");
    }
    const auto kind = field(decision.args, "kind");
    const bool work = kind == "work";
    bool charge = false;
    if (work) {
        if (sender.activeRun || !sender.actionId.empty())
            throw Conflict("Let the assigned Run settle before requesting another Bot's work");
        peer.intake.environmentId = db.defaultT3Environment(peer.intake.botId, peer.intake.environmentId, sender.environmentId);
        if (peer.intake.environmentId.empty()) throw Conflict("Choose a T3 environment for the destination Bot in its configuration");
        if (!peer.intake.projectId.empty() && db.peerWorkRouteWouldCycle(sender.id, peer.intake.environmentId, peer.intake.projectId))
            throw Conflict("This work request would make two project queues wait for one another");
        if (!updates.admitWork(sender, decision, charge)) return;
        updates.establishCriteria(sender, args);
    }
    peer.intake.request = note.empty() && forwarded ? forwarded->text : note;
    const auto source = work ? TaskSource::handoff : TaskSource::consultation;
    const auto id = db.submit(peer.intake, 0, nullptr, source);
    auto recipient = db.load(id);
    const PeerExchange exchange{action, kind, "working", "", sender.id, id};
    auto message = peerMessage(exchange, sender, recipient, "request", note, action);
    message.chargesBudget = charge;
    message.forwarded = std::move(forwarded);
    transitionPeer(exchange, sender, recipient, message, message.forwarded ? sender.sourceMessageId : std::nullopt);
    updates.recordDecision(sender, event, decision, action);
}

void Runtime::PeerExchanges::finishPeer(const PeerExchange& exchange, TaskData& recipient, TaskData sender, std::string_view status,
                                        std::string_view text, const std::string& action) {
    const auto type = status == "completed" ? std::string_view{"result"} : status;
    auto message = peerMessage(exchange, recipient, sender, type, text, action);
    message.status = status;
    if (const auto report = status != "cancelled" ? reports.settledSessionReport(recipient) : std::nullopt) {
        const auto& data = report->data;
        message.evidence.push_back(
            {data.environmentId, data.run.threadId, data.resultRunId, data.evidence.itemId, data.run.link, data.status});
    }
    transitionPeer(exchange, recipient, sender, message);
}

Json Runtime::PeerExchanges::replyKinds(const TaskData& task, const PeerExchange& peer) const {
    if (peer.sender == task.id && peer.phase == "question") return Json::array({"answer"});
    if (peer.recipient != task.id || peer.phase != "working") return Json::array();
    Json kinds = {"question"};
    const bool revisedWork = peer.kind == "work" && (!task.pendingInstructions.empty() || task.replanning || task.newSessionRequired ||
                                                     (task.lastRun && task.lastRun->assignment.revisionId != task.currentRevision));
    if (!revisedWork) {
        kinds.push_back("failed");
        if (!reports.sessionReportUnavailable(task)) kinds.push_back("result");
    }
    return kinds;
}

bool Runtime::PeerExchanges::replyPeer(TaskData& task, const Json& event, const TaskDecision& decision, const std::string& action) {
    const auto peer = db.peerExchange(task.id);
    if (!peer) throw Conflict("The Bot exchange has closed");
    const auto type = field(decision.args, "kind");
    const auto body = field(decision.args, "text");
    const auto kinds = replyKinds(task, *peer);
    if (std::find(kinds.begin(), kinds.end(), type) == kinds.end())
        throw Conflict("This reply does not match the open exchange and current task revision");
    const bool answering = type == "answer";
    const auto otherId = answering ? peer->recipient : peer->sender;
    auto other = db.load(otherId);
    if (db.botPaused(other.botId)) throw BotPaused{};
    if (terminal(other) || other.state != TaskState::peer_waiting) throw Conflict("The other Bot is no longer waiting");
    const bool closing = type == "result" || type == "failed";
    if (closing && db.conversationPending(task.botId)) return false;
    updates.recordDecision(task, event, decision, action);
    if (closing) {
        const auto outcome = type == "result" ? TaskPeerOutcome::completed : TaskPeerOutcome::failed;
        const auto status = taskPeerOutcomeName(outcome);
        task.state = TaskState::completed;
        task.peerOutcome = outcome;
        finishPeer(*peer, task, std::move(other), status, body, action);
        db.journal(task.id, "peer_completed",
                   {{"text", body}, {"status", status}, {"reason", decision.reason}, {"doneWhen", task.doneWhen}});
    } else {
        transitionPeer(*peer, task, other, peerMessage(*peer, task, other, type, body, action));
    }
    return true;
}

void Runtime::PeerExchanges::readPeerEvidence(const TaskData& task, const Json& event, const TaskDecision& decision) {
    const auto& args = decision.args;
    const auto messageId = field(args, "messageId");
    if (event["kind"] != "bot_result" || messageId.empty() || event["messageId"] != messageId)
        throw Conflict("Choose the Bot result for the current exchange");
    allowedPeer(task.botId, field(event, "fromBotId"));
    const auto refs = event.value("references", Json::array());
    if (refs.empty()) throw Conflict("The Bot result has no shared Session history");
    const auto& ref = refs.front();
    ReadRequest req;
    req.environmentId = field(ref, "environmentId");
    req.threadId = field(ref, "threadId");
    req.runId = field(ref, "runId");
    req.itemId = field(args, "itemId");
    req.cursor = field(args, "cursor");
    req.offset = SessionHistory::readOffset(args);
    {
        std::lock_guard lock(db.mutex);
        auto row = db.query("SELECT 1 FROM peer_exchanges WHERE id=? AND sender_task=? AND last_message=?",
                            {event["exchangeId"], task.id, messageId});
        if (!row.next()) throw Conflict("The Bot result does not belong to this task");
        req.config = task.config;
    }
    if (req.threadId.empty() || req.runId.empty()) throw Conflict("The Bot result has no readable Run");
    if (!policy.allows(task.botId, Resource::sessionRead(req.environmentId, req.threadId))) throw Conflict("Session read access required");
    const PeerEvidenceEntry entry{event.at("exchangeId").get<std::string>(), messageId, history.readExcerpt(task.botId, req)};
    updates.change(task, [&](TaskData& current) {
        history.appendEventEvidence(current, event, encodePeerEvidence(entry));
        db.journal(current.id, entry, decision.usage);
        return true;
    });
}

std::optional<PeerCompletion> Runtime::PeerExchanges::peerCompletion(const TaskData& task, const Json& event) {
    const auto peer = peerResult(task, event);
    if (!peer) return std::nullopt;

    PeerCompletion result{peer->taskId,
                          event.at("exchangeId").get<std::string>(),
                          event.at("messageId").get<std::string>(),
                          field(event, "fromBotId"),
                          peer->kind,
                          event.at("status").get<std::string>(),
                          {}};
    const auto allowed = peersFor(task.botId);
    const bool peerAllowed = std::ranges::any_of(allowed, [&](const PeerBot& candidate) { return candidate.intake.botId == result.botId; });
    const auto addRef = [&](CompletionEvidence ref, bool fromPeer) {
        if ((fromPeer && !peerAllowed) || ref.runId.empty() ||
            !policy.allows(task.botId, Resource::sessionRead(ref.environmentId, ref.threadId)))
            return;
        if (ref.itemId && ref.itemId->empty()) ref.itemId.reset();
        if (fromPeer) ref.messageId = result.messageId;
        if (std::ranges::find(result.evidence, ref) == result.evidence.end()) result.evidence.push_back(std::move(ref));
    };
    const auto addEventRef = [&](const Json& record, CompletionEvidenceKind kind, bool fromPeer) {
        addRef({field(record, "environmentId"), field(record, "threadId"), field(record, "runId"), kind, field(record, "itemId"), {}},
               fromPeer);
    };
    for (const auto& record : event.value("references", Json::array())) addEventRef(record, CompletionEvidenceKind::peer_report, true);
    auto reads = db.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='read_bot_evidence'
  AND exchange_id=? AND peer_message_id=? ORDER BY id
)SQL",
                          {task.id, result.exchangeId, result.messageId});
    while (reads.next()) {
        auto record = decodePeerEvidence(reads.number(0), reads.json(1));
        auto& excerpt = record.excerpt;
        addRef({std::move(excerpt.environmentId),
                std::move(excerpt.threadId),
                std::move(excerpt.runId),
                CompletionEvidenceKind::session_read,
                std::move(excerpt.itemId),
                {}},
               true);
    }
    for (const auto& record : event.at("evidence"))
        if (!record.contains("exchangeId")) addEventRef(record, CompletionEvidenceKind::session_read, false);
    return result;
}

bool Runtime::PeerExchanges::forwardedQuestionAnswer(const TaskData& task, const QuestionTarget& target) {
    std::lock_guard lock(db.mutex);
    const auto event = db.currentEvent(task);
    const auto peer = db.peerExchange(task.id);
    if (!peer || peer->recipient != task.id || peer->kind != "work" || peer->phase != "working" || event.is_null() ||
        event["consumed"] == true || field(event, "kind") != "bot_answer" || field(event, "messageType") != "answer" ||
        field(event, "requestKind") != peer->kind || field(event, "exchangeId") != peer->id ||
        field(event, "messageId") != peer->lastMessage || field(event, "toBotId") != task.botId)
        return false;
    const auto sender = db.load(peer->sender);
    if (field(event, "fromBotId") != sender.botId) return false;
    auto original = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {target.eventId, task.id});
    if (!original.next()) return false;
    const auto question = original.json(0);
    const auto questionRun = question.value("run", Json::object());
    return target.run && field(question, "kind") == "question" && field(questionRun, "threadId") == target.run->run.threadId &&
           field(questionRun, "runId") == target.run->run.runId &&
           field(question.value("question", Json::object()), "requestId") == target.requestId;
}
}
