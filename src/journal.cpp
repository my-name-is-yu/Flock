#include "journal.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/wait.hpp"
#include "flock/outbox_instruction.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
std::optional<std::string_view> gatewayId(const RunRef& run) {
    if (run.gatewayEnvironmentId.empty()) return std::nullopt;
    return run.gatewayEnvironmentId;
}

RunReportEntry readRunReport(const Json& data, const RecordDecoder& row) {
    if (row.get<std::string>(data, "kind") != "result") row.fail("kind", "expected a Run result");
    RunReportEntry entry{.environmentId = row.get<std::string>(data, "environmentId"),
                         .resultRunId = row.get<std::string>(data, "resultRunId"),
                         .status = row.get<std::string>(data, "status"),
                         .revisionId = row.nullable<Id>(data, "revisionId"),
                         .evidence = {.availability = decodeReportAvailability(data, row),
                                      .report = row.get<std::string>(data, "report"),
                                      .itemId = row.get<std::string>(data, "itemId"),
                                      .detail = row.get<std::string>(data, "detail"),
                                      .truncated = row.get<bool>(data, "truncated"),
                                      .sessionCreatedAt = row.optional<std::string>(data, "sessionCreatedAt").value_or("")},
                         .stopRequested = row.optional<bool>(data, "stopRequested")};
    const auto& run = row.require(data, "run");
    const auto reader = row.nested("run");
    if (auto destination = row.optional<std::string>(data, "destination")) {
        entry.run = reader.decodeClosed(run, decodeRunRef);
        entry.details =
            DestinationRunReportDetails{std::move(*destination), row.get<std::string>(data, "projectId"),
                                        row.get<std::string>(data, "commit"), row.optional<std::string>(data, "requestedCommit")};
    } else {
        auto assigned = reader.decodeClosed(run, decodeAssignedRun);
        if (entry.environmentId != assigned.environmentId) row.fail("environmentId", "does not match the assigned Run");
        if (entry.revisionId != assigned.assignment.revisionId) row.fail("revisionId", "does not match the assigned Run");
        entry.run = std::move(assigned.ref);
        entry.details = AssignedRunReportDetails{std::move(assigned.assignment.ownerBotId), std::move(assigned.assignment.projectId)};
    }
    return entry;
}
}

UserAnswerEntry decodeUserAnswer(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json& payload, const RecordDecoder& row) {
        const auto message = row.optional<Id>(payload, "messageId");
        if (row.get<bool>(payload, "conversation") != message.has_value()) row.fail("conversation", "does not match the message link");
        return UserAnswerEntry{row.get<std::string>(payload, "text"), row.get<Id>(payload, "waitId"), message};
    });
}

Json encodeUserAnswer(const UserAnswerEntry& entry) {
    Json data{{"text", entry.text}, {"waitId", entry.waitId}, {"conversation", entry.messageId.has_value()}};
    if (entry.messageId) data["messageId"] = *entry.messageId;
    return data;
}

RunReportEntry runReportEntry(const SettledRunReport& report) {
    return {report.run.ref,
            report.run.environmentId,
            report.resultRunId,
            report.status,
            report.run.assignment.revisionId,
            boundedReportEvidence(report.evidence),
            report.stopRequested ? std::optional{true} : std::nullopt,
            AssignedRunReportDetails{report.run.assignment.ownerBotId, report.run.assignment.projectId}};
}

bool RunReportEntry::matches(const AssignedRun& assigned) const {
    const auto* owner = std::get_if<AssignedRunReportDetails>(&details);
    return owner && run == assigned.ref && environmentId == assigned.environmentId && revisionId == assigned.assignment.revisionId &&
           owner->ownerBotId == assigned.assignment.ownerBotId && owner->projectId == assigned.assignment.projectId;
}

RunReportEntry decodeRunReport(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, readRunReport);
}

SettledRunReport decodeSettledRunReport(const Json& data, const RecordDecoder& row) {
    auto entry = row.decodeClosed(data, readRunReport);
    auto* assignment = std::get_if<AssignedRunReportDetails>(&entry.details);
    if (!assignment) row.fail("run", "expected an assigned Run");
    return {{std::move(entry.run),
             std::move(entry.environmentId),
             {std::move(assignment->ownerBotId), std::move(assignment->projectId), entry.revisionId}},
            std::move(entry.resultRunId),
            std::move(entry.status),
            std::move(entry.evidence),
            entry.stopRequested.value_or(false)};
}

Json runReportRunJson(const RunReportEntry& report) {
    auto data = runJson(report.run);
    if (const auto* assignment = std::get_if<AssignedRunReportDetails>(&report.details)) {
        data["environmentId"] = report.environmentId;
        encodeRunAssignment(data, {assignment->ownerBotId, assignment->projectId, report.revisionId});
    }
    return data;
}

OutboxSourceRun reportSourceRun(const RunReportEntry& report) {
    OutboxSourceRun source{.threadId = report.run.threadId, .runId = report.run.runId, .link = report.run.link};
    if (const auto gateway = gatewayId(report.run)) source.gatewayEnvironmentId = *gateway;
    if (const auto* assignment = std::get_if<AssignedRunReportDetails>(&report.details)) {
        source.environmentId = report.environmentId;
        source.assignment = RunAssignment{assignment->ownerBotId, assignment->projectId, report.revisionId};
    }
    return source;
}

Json encodeRunReport(const RunReportEntry& report) {
    Json data{{"kind", "result"},
              {"run", runReportRunJson(report)},
              {"environmentId", report.environmentId},
              {"resultRunId", report.resultRunId},
              {"status", report.status},
              {"revisionId", report.revisionId ? Json(*report.revisionId) : Json(nullptr)}};
    if (report.stopRequested) data["stopRequested"] = *report.stopRequested;
    if (const auto* destination = std::get_if<DestinationRunReportDetails>(&report.details)) {
        data["destination"] = destination->name;
        data["projectId"] = destination->projectId;
        data["commit"] = destination->commit;
        if (destination->requestedCommit) data["requestedCommit"] = *destination->requestedCommit;
    }
    setReportEvidence(data, report.evidence);
    return data;
}

Id Store::Impl::journal(Id task, const Wait& wait) {
    const auto reason = wait.reason();
    auto data = encodeWaitData(wait);
    data["reason"] = waitReasonName(reason);
    data["id"] = wait.id;
    if (wait.allowance) data["allowance"] = *wait.allowance;
    const auto id = writeJournal(task, JournalKind::waiting, data, {.waitId = wait.id, .waitReason = reason});
    if (reason == WaitReason::user_question) queueChannelTaskUpdate(task, id, wait.text, wait.choices());
    return id;
}

Id Store::Impl::journal(Id task, const UserAnswerEntry& entry) {
    return writeJournal(task, JournalKind::user_answer, encodeUserAnswer(entry),
                        {.waitId = entry.waitId, .answerInConversation = entry.messageId.has_value()});
}

OutboxInstruction decodeAcceptedInstruction(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return decodeOutboxInstruction(data, {"journal", rowId});
}

Id Store::Impl::journal(Id task, const OutboxInstruction& entry) {
    return writeJournal(task, JournalKind::instruction_accepted, instructionJson(entry),
                        {.messageId = entry.messageId, .instructionStep = entry.step});
}

Id Store::Impl::journal(Id task, const DeliveredInstructionEntry& entry) {
    const auto& instruction = entry.instruction;
    return writeJournal(task, JournalKind::instruction_delivered,
                        {{"actionId", entry.actionId},
                         {"messageId", instruction.messageId},
                         {"step", instruction.step},
                         {"revisionId", entry.revisionId ? Json(*entry.revisionId) : Json(nullptr)},
                         {"run", assignedRunJson(entry.run)},
                         {"source", instruction.source ? sourceJson(*instruction.source) : Json(nullptr)}},
                        {.revisionId = entry.revisionId, .messageId = instruction.messageId, .instructionStep = instruction.step});
}

Id Store::Impl::journal(Id task, const TaskRevisionEntry& entry) {
    return writeJournal(task, JournalKind::task_revision, {{"revisionId", entry.revisionId}, {"sourceMessageId", entry.sourceMessageId}},
                        {.revisionId = entry.revisionId});
}

Id Store::Impl::journal(Id task, const SendStartedEntry& entry) {
    return writeJournal(task, JournalKind::send_started, {{"actionId", entry.actionId}}, {.actionId = entry.actionId});
}

Id Store::Impl::journal(Id task, const RunReportEntry& entry) {
    return writeRunReportJournal(task, JournalKind::report, entry);
}

Id Store::Impl::journalRunSettled(Id task, const SettledRunReport& entry) {
    return writeRunReportJournal(task, JournalKind::run_settled, runReportEntry(entry));
}

Id Store::Impl::writeRunReportJournal(Id task, JournalKind kind, const RunReportEntry& entry) {
    return writeJournal(task, kind, encodeRunReport(entry),
                        {.revisionId = entry.revisionId,
                         .stopRequested = entry.stopRequested,
                         .run = &entry.run,
                         .environmentId = entry.environmentId,
                         .resultRunId = entry.resultRunId,
                         .status = entry.status});
}

Id Store::Impl::journalInterrupt(Id task, const RunStop& stop) {
    JournalKeys keys;
    if (stop.run) {
        keys.run = &stop.run->ref;
        keys.environmentId = stop.run->environmentId;
    }
    return writeJournal(task, JournalKind::interrupt_requested, runStopJson(stop), keys);
}

Id Store::Impl::journalInterrupt(Id task, const ConversationInterruptEntry& entry) {
    return writeJournal(task, JournalKind::interrupt_requested,
                        {{"messageId", entry.messageId}, {"step", entry.step}, {"run", assignedRunJson(entry.run)}},
                        {.run = &entry.run.ref, .environmentId = entry.run.environmentId});
}
}
