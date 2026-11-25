#include "journal.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "storage.hpp"
#include "task_storage.hpp"

namespace flockv0 {
namespace {
constexpr std::array kinds{
    std::pair{JournalKind::waiting, std::string_view{"waiting"}},
    std::pair{JournalKind::user_answer, std::string_view{"user_answer"}},
    std::pair{JournalKind::send_started, std::string_view{"send_started"}},
    std::pair{JournalKind::report, std::string_view{"report"}},
    std::pair{JournalKind::run_settled, std::string_view{"run_settled"}},
    std::pair{JournalKind::interrupt_requested, std::string_view{"interrupt_requested"}},
    std::pair{JournalKind::instruction_accepted, std::string_view{"instruction_accepted"}},
    std::pair{JournalKind::instruction_delivered, std::string_view{"instruction_delivered"}},
    std::pair{JournalKind::task_revision, std::string_view{"task_revision"}},
    std::pair{JournalKind::bot_message, std::string_view{"bot_message"}},
    std::pair{JournalKind::read_bot_evidence, std::string_view{"read_bot_evidence"}},
    std::pair{JournalKind::pr_review_requested, std::string_view{"pr_review_requested"}},
    std::pair{JournalKind::git_pr_followup_requested, std::string_view{"git_pr_followup_requested"}},
    std::pair{JournalKind::conversation_message, std::string_view{"conversation_message"}},
    std::pair{JournalKind::conversation_reply, std::string_view{"conversation_reply"}},
    std::pair{JournalKind::conversation_budget_approved, std::string_view{"conversation_budget_approved"}},
    std::pair{JournalKind::completed, std::string_view{"completed"}},
    std::pair{JournalKind::cancel_requested, std::string_view{"cancel_requested"}},
    std::pair{JournalKind::cancelled, std::string_view{"cancelled"}},
    std::pair{JournalKind::tool_error, std::string_view{"tool_error"}},
    std::pair{JournalKind::decision, std::string_view{"decision"}},
};

template <class T> Json sqlValue(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json sqlFlag(std::optional<bool> value) {
    return value ? Json(*value ? 1 : 0) : Json(nullptr);
}

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
              {"revisionId", sqlValue(report.revisionId)}};
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

Id Store::Impl::writeJournal(std::optional<Id> task, JournalKind kind, const Json& data, const JournalKeys& keys,
                             std::optional<std::string_view> conversationBot) {
    const auto* run = keys.run;
    exec(R"SQL(
INSERT INTO journal(task_id,time,kind,data,conversation_bot_id,wait_id,wait_reason,user_answer_conversation,
                    action_id,run_environment_id,run_thread_id,run_id,run_gateway_id,result_run_id,run_status,revision_id,stop_requested,
                    message_id,instruction_step,exchange_id,peer_message_id,forwarded_source_message_id,pr_review_item_id,
                    git_event_id,operation_id,reply_to_message_id,task_question_message_id,wait_message_id,conversation_route,
                    conversation_status,session_participation_id,session_stop_id,feedback_event_id,workflow_outcome)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL",
         {sqlValue(task),
          unixTime(),
          enumName(kinds, kind),
          data,
          sqlValue(conversationBot),
          sqlValue(keys.waitId),
          keys.waitReason ? Json(waitReasonName(*keys.waitReason)) : Json(nullptr),
          sqlFlag(keys.answerInConversation),
          sqlValue(keys.actionId),
          sqlValue(keys.environmentId),
          run ? Json(run->threadId) : Json(nullptr),
          run ? Json(run->runId) : Json(nullptr),
          run ? sqlValue(gatewayId(*run)) : Json(nullptr),
          sqlValue(keys.resultRunId),
          sqlValue(keys.status),
          sqlValue(keys.revisionId),
          sqlFlag(keys.stopRequested),
          sqlValue(keys.messageId),
          sqlValue(keys.instructionStep),
          sqlValue(keys.exchangeId),
          sqlValue(keys.peerMessageId),
          sqlValue(keys.forwardedSourceMessageId),
          sqlValue(keys.prReviewItemId),
          sqlValue(keys.gitEventId),
          sqlValue(keys.operationId),
          sqlValue(keys.replyToMessageId),
          sqlValue(keys.taskQuestionMessageId),
          sqlValue(keys.waitMessageId),
          sqlValue(keys.conversationRoute),
          sqlValue(keys.conversationStatus),
          sqlValue(keys.sessionParticipationId),
          sqlValue(keys.sessionStopId),
          sqlValue(keys.feedbackEventId),
          sqlValue(keys.workflowOutcome)});
    return last();
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
                         {"revisionId", sqlValue(entry.revisionId)},
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
