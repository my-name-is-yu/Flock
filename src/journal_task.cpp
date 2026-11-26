#include "session_stop.hpp"
#include "journal_storage.hpp"
#include "channel_storage.hpp"
#include "journal_task.hpp"
#include "journal.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/outbox_instruction.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
using namespace std::literals;
constexpr std::array evidenceKinds{
    std::pair{CompletionEvidenceKind::peer_report, "peer_report"sv},
    std::pair{CompletionEvidenceKind::session_read, "session_read"sv},
};
constexpr std::array workflowOutcomes{
    std::pair{WorkflowCompletionOutcome::completed, "completed"sv},
    std::pair{WorkflowCompletionOutcome::failed, "failed"sv},
};

Json evidenceJson(const CompletionEvidence& evidence) {
    Json data{{"environmentId", evidence.environmentId},
              {"threadId", evidence.threadId},
              {"runId", evidence.runId},
              {"kind", enumName(evidenceKinds, evidence.kind)}};
    if (evidence.itemId) data["itemId"] = *evidence.itemId;
    if (evidence.messageId) data["messageId"] = *evidence.messageId;
    return data;
}

CompletionEvidence readEvidence(const Json& data, const RecordDecoder& row) {
    const auto kind = enumValue(evidenceKinds, row.get<std::string>(data, "kind"));
    if (!kind) row.fail("kind", "unknown completion evidence kind");
    auto messageId = row.optional<std::string>(data, "messageId");
    if (*kind == CompletionEvidenceKind::peer_report && !messageId) row.fail("messageId", "required for a peer report");
    return {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "threadId"),
            row.get<std::string>(data, "runId"),         *kind,
            row.optional<std::string>(data, "itemId"),   std::move(messageId)};
}

CompletionProvenance readCompletionProvenance(const Json& data, const RecordDecoder& row) {
    if (const auto source = row.find(data, "source"); source != data.end()) return decodeOutboxSourceRun(*source, row.nested("source"));
    if (const auto dependency = row.find(data, "dependency"); dependency != data.end()) {
        auto peer = row.nested("dependency").decodeClosed(*dependency, [](const Json& value, const RecordDecoder& ref) {
            return PeerCompletion{ref.get<Id>(value, "taskId"),
                                  ref.get<std::string>(value, "exchangeId"),
                                  ref.get<std::string>(value, "messageId"),
                                  ref.get<std::string>(value, "botId"),
                                  ref.get<std::string>(value, "requestKind"),
                                  ref.get<std::string>(value, "status"),
                                  {}};
        });
        const auto& evidence = row.array(row.require(data, "evidenceRefs"), "evidenceRefs");
        const auto evidenceRow = row.nested("evidenceRefs");
        for (const auto& ref : evidence)
            peer.evidence.push_back(evidenceRow.nested(std::to_string(peer.evidence.size())).decodeClosed(ref, readEvidence));
        return peer;
    }
    return std::monostate{};
}

void encodeTaskDetails(Json& data, const TaskCompletion& task) {
    data["reason"] = task.reason;
    data["doneWhen"] = task.doneWhen;
    if (const auto* source = std::get_if<OutboxSourceRun>(&task.provenance)) data["source"] = sourceRunJson(*source);
    if (const auto* peer = std::get_if<PeerCompletion>(&task.provenance)) {
        data["dependency"] = {{"taskId", peer->taskId}, {"exchangeId", peer->exchangeId},   {"messageId", peer->messageId},
                              {"botId", peer->botId},   {"requestKind", peer->requestKind}, {"status", peer->status}};
        auto refs = Json::array();
        for (const auto& ref : peer->evidence) refs.push_back(evidenceJson(ref));
        data["evidenceRefs"] = std::move(refs);
    }
}

CompletionEntry readCompletion(const Json& data, const RecordDecoder& row) {
    auto text = row.get<std::string>(data, "text");
    const auto revision = row.nullable<Id>(data, "revisionId");
    if (const auto outcome = row.optional<std::string>(data, "runOutcome"))
        return {std::move(text), revision,
                FanoutCompletion{row.get<std::string>(data, "doneWhen"), decodeRunOutcome(*outcome, row),
                                 row.optional<std::string>(data, "requestedCommit")}};
    if (auto reason = row.optional<std::string>(data, "reason"))
        return {std::move(text), revision,
                TaskCompletion{std::move(*reason), row.get<std::string>(data, "doneWhen"), readCompletionProvenance(data, row)}};
    auto run = decodeOutboxSourceRun(row.require(data, "source"), row.nested("source"));
    std::optional<WorkflowCompletionOutcome> outcome;
    if (const auto state = row.optional<std::string>(data, "workflowOutcome")) {
        outcome = enumValue(workflowOutcomes, *state);
        if (!outcome) row.fail("workflowOutcome", "unknown workflow outcome");
    }
    return {std::move(text), revision, WorkflowCompletion{std::move(run), outcome}};
}
}

const OutboxSourceRun* CompletionEntry::source() const {
    if (const auto* task = std::get_if<TaskCompletion>(&details)) return std::get_if<OutboxSourceRun>(&task->provenance);
    if (const auto* workflow = std::get_if<WorkflowCompletion>(&details)) return &workflow->source;
    return nullptr;
}

CompletionEntry decodeCompletion(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, readCompletion);
}

Json encodeCompletion(const CompletionEntry& entry) {
    Json data{{"text", entry.text}, {"revisionId", entry.revisionId ? Json(*entry.revisionId) : Json(nullptr)}};
    if (const auto* task = std::get_if<TaskCompletion>(&entry.details)) {
        encodeTaskDetails(data, *task);
    } else if (const auto* fanout = std::get_if<FanoutCompletion>(&entry.details)) {
        data["doneWhen"] = fanout->doneWhen;
        data["runOutcome"] = runOutcomeName(fanout->outcome);
        if (fanout->requestedCommit) data["requestedCommit"] = *fanout->requestedCommit;
    } else {
        const auto& workflow = std::get<WorkflowCompletion>(entry.details);
        data["source"] = sourceRunJson(workflow.source);
        if (workflow.outcome) data["workflowOutcome"] = enumName(workflowOutcomes, *workflow.outcome);
    }
    return data;
}

ToolErrorEntry decodeToolError(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json& payload, const RecordDecoder& row) {
        auto [error, unknown] =
            row.nested("result").decodeClosed(row.require(payload, "result"), [](const Json& result, const RecordDecoder& ref) {
                if (ref.get<bool>(result, "ok")) ref.fail("ok", "expected a failed tool result");
                const auto delivery = ref.optional<std::string>(result, "delivery");
                if (delivery && *delivery != "unknown") ref.fail("delivery", "unknown tool delivery state");
                return std::pair{ref.get<std::string>(result, "error"), delivery.has_value()};
            });
        ToolErrorEntry entry{row.get<Id>(payload, "eventId"), row.get<std::string>(payload, "tool"), std::move(error), unknown, {}};
        if (const auto args = row.find(payload, "args"); args != payload.end())
            entry.arguments = *args;
        else
            entry.arguments = row.get<std::string>(payload, "argumentsExcerpt");
        return entry;
    });
}

Json encodeToolError(const ToolErrorEntry& entry) {
    Json result{{"ok", false}, {"error", entry.error}};
    if (entry.unknownDelivery) result["delivery"] = "unknown";
    Json data{{"eventId", entry.eventId}, {"tool", entry.tool}, {"result", std::move(result)}};
    if (const auto* args = std::get_if<Json>(&entry.arguments))
        data["args"] = *args;
    else
        data["argumentsExcerpt"] = std::get<std::string>(entry.arguments);
    return data;
}

DecisionEntry decodeJournalDecision(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json& payload, const RecordDecoder& row) {
        return DecisionEntry{row.get<Id>(payload, "eventId"),
                             row.nullable<Id>(payload, "revisionId"),
                             row.get<std::string>(payload, "actionId"),
                             {row.get<std::string>(payload, "tool"), row.require(payload, "args"), row.get<std::string>(payload, "reason")},
                             row.array(row.require(payload, "evidence"), "evidence"),
                             row.require(payload, "usage")};
    });
}

Json encodeJournalDecision(const DecisionEntry& entry) {
    return {{"eventId", entry.eventId},    {"revisionId", entry.revisionId ? Json(*entry.revisionId) : Json(nullptr)},
            {"actionId", entry.actionId},  {"tool", entry.decision.tool},
            {"args", entry.decision.args}, {"reason", entry.decision.reason},
            {"evidence", entry.evidence},  {"usage", entry.usage}};
}

Id JournalStorage::journal(Id task, const CompletionEntry& entry) {
    JournalKeys keys{.revisionId = entry.revisionId};
    if (const auto* workflow = std::get_if<WorkflowCompletion>(&entry.details); workflow && workflow->outcome)
        keys.workflowOutcome = enumName(workflowOutcomes, *workflow->outcome);
    const auto id = database.writeJournal(task, JournalKind::completed, encodeCompletion(entry), keys);
    channelStorage.queueChannelTaskUpdate(task, id, entry.text);
    return id;
}

Id JournalStorage::journal(Id task, const ToolErrorEntry& entry) {
    return database.writeJournal(task, JournalKind::tool_error, encodeToolError(entry), {.feedbackEventId = entry.eventId});
}

Id JournalStorage::journal(Id task, const DecisionEntry& entry) {
    return database.writeJournal(task, JournalKind::decision, encodeJournalDecision(entry), {});
}

Id JournalStorage::journalCancelRequested(Id task, SessionStop stop) {
    if (!stop.participation) return database.writeJournal(task, JournalKind::cancel_requested, Json::object(), {});
    return database.writeJournal(task, JournalKind::cancel_requested,
                                 {{"sessionParticipationId", stop.participation}, {"sessionStopId", stop.cutoff}},
                                 {.sessionParticipationId = stop.participation, .sessionStopId = stop.cutoff});
}

Id JournalStorage::journalCancelled(Id task) {
    const auto id = database.writeJournal(task, JournalKind::cancelled, Json::object(), {});
    channelStorage.queueChannelTaskUpdate(task, id, "The task was cancelled.");
    return id;
}
}
