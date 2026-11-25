#pragma once

#include "flock/core.hpp"
#include "flock/outbox_instruction.hpp"

namespace flockv0 {
class RecordDecoder;

enum class JournalKind { waiting, user_answer, send_started, report, run_settled, interrupt_requested };

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
}
