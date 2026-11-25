#pragma once

#include "flock/core.hpp"
#include "flock/outbox_instruction.hpp"

namespace flockv0 {
class RecordDecoder;

enum class JournalKind {
    waiting,
    user_answer,
    send_started,
    report,
    run_settled,
    interrupt_requested,
    instruction_accepted,
    instruction_delivered,
    task_revision,
    bot_message,
    read_bot_evidence,
    pr_review_requested,
    git_pr_followup_requested,
    conversation_message,
    conversation_reply,
    conversation_budget_approved,
    completed,
    cancel_requested,
    cancelled,
    tool_error,
    decision
};

struct JournalKeys {
    std::optional<Id> waitId, revisionId;
    std::optional<WaitReason> waitReason;
    std::optional<bool> answerInConversation, stopRequested;
    const RunRef* run = nullptr;
    std::optional<std::string_view> environmentId, resultRunId, status, actionId;
    std::optional<Id> messageId;
    std::optional<int> instructionStep;
    std::optional<std::string_view> exchangeId, peerMessageId;
    std::optional<Id> forwardedSourceMessageId, prReviewItemId;
    std::optional<std::string_view> gitEventId, operationId;
    std::optional<Id> replyToMessageId, taskQuestionMessageId, waitMessageId;
    std::optional<std::string_view> conversationRoute, conversationStatus;
    std::optional<Id> sessionParticipationId, sessionStopId, feedbackEventId;
    std::optional<std::string_view> workflowOutcome;
};

struct TaskRevisionEntry {
    Id revisionId, sourceMessageId;
};

struct DeliveredInstructionEntry {
    std::string_view actionId;
    const OutboxInstruction& instruction;
    std::optional<Id> revisionId;
    const std::optional<AssignedRun>& run;
};

struct UserAnswerEntry {
    std::string text;
    Id waitId;
    std::optional<Id> messageId;
};

struct SendStartedEntry {
    std::string_view actionId;
};

struct ConversationInterruptEntry {
    Id messageId;
    int step;
    const AssignedRun& run;
};

struct AssignedRunReportDetails {
    std::string ownerBotId, projectId;
    bool operator==(const AssignedRunReportDetails&) const = default;
};

struct DestinationRunReportDetails {
    std::string name, projectId, commit;
    std::optional<std::string> requestedCommit;
    bool operator==(const DestinationRunReportDetails&) const = default;
};

struct RunReportEntry {
    RunRef run;
    std::string environmentId, resultRunId, status;
    std::optional<Id> revisionId;
    RunEvidence evidence;
    std::optional<bool> stopRequested;
    std::variant<AssignedRunReportDetails, DestinationRunReportDetails> details;
    bool matches(const AssignedRun&) const;
    bool operator==(const RunReportEntry&) const = default;
};

UserAnswerEntry decodeUserAnswer(Id, const Json&);
Json encodeUserAnswer(const UserAnswerEntry&);
RunReportEntry runReportEntry(const SettledRunReport&);
RunReportEntry decodeRunReport(Id, const Json&);
SettledRunReport decodeSettledRunReport(const Json&, const RecordDecoder&);
Json encodeRunReport(const RunReportEntry&);
Json runReportRunJson(const RunReportEntry&);
OutboxSourceRun reportSourceRun(const RunReportEntry&);
OutboxInstruction decodeAcceptedInstruction(Id, const Json&);
}
