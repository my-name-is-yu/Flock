#include "session_stop.hpp"
#include "journal_storage.hpp"
#include "channel_storage.hpp"
#include "journal_task.hpp"
#include "journal.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/outbox_instruction.hpp"
#include "database.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
void encodeTaskDetails(Json& data, const TaskCompletion& task) {
    data["reason"] = task.reason;
    data["doneWhen"] = task.doneWhen;
    if (task.source) data["source"] = sourceRunJson(*task.source);
}

CompletionEntry readCompletion(const Json& data, const RecordDecoder& row) {
    auto text = row.get<std::string>(data, "text");
    const auto revision = row.nullable<Id>(data, "revisionId");
    if (const auto outcome = row.optional<std::string>(data, "runOutcome"))
        return {std::move(text), revision,
                FanoutCompletion{row.get<std::string>(data, "doneWhen"), decodeRunOutcome(*outcome, row),
                                 row.optional<std::string>(data, "requestedCommit")}};
    const auto readSource = [](const Json& data, const RecordDecoder& row) -> std::optional<OutboxSourceRun> {
        if (const auto source = row.find(data, "source"); source != data.end()) return decodeOutboxSourceRun(*source, row.nested("source"));
        return std::nullopt;
    };
    return {std::move(text), revision,
            TaskCompletion{row.get<std::string>(data, "reason"), row.get<std::string>(data, "doneWhen"), readSource(data, row)}};
}
}

const OutboxSourceRun* CompletionEntry::source() const {
    if (const auto* task = std::get_if<TaskCompletion>(&details); task && task->source) return &*task->source;
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
    } else {
        const auto& fanout = std::get<FanoutCompletion>(entry.details);
        data["doneWhen"] = fanout.doneWhen;
        data["runOutcome"] = runOutcomeName(fanout.outcome);
        if (fanout.requestedCommit) data["requestedCommit"] = *fanout.requestedCommit;
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
    const auto id = database.writeJournal(task, JournalKind::completed, encodeCompletion(entry), {.revisionId = entry.revisionId});
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
