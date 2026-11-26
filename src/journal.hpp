#pragma once

#include "flock/outbox_instruction.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/wait.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace flockv0 {
class RecordDecoder;

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
