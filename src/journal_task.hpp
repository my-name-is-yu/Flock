#pragma once

#include "flock/core.hpp"
#include "flock/outbox_instruction.hpp"

namespace flockv0 {
enum class CompletionEvidenceKind { peer_report, session_read };

struct CompletionEvidence {
    std::string environmentId, threadId, runId;
    CompletionEvidenceKind kind;
    std::optional<std::string> itemId, messageId;
    bool operator==(const CompletionEvidence&) const = default;
};

struct PeerCompletion {
    Id taskId;
    std::string exchangeId, messageId, botId, requestKind, status;
    std::vector<CompletionEvidence> evidence;
};

using CompletionProvenance = std::variant<std::monostate, OutboxSourceRun, PeerCompletion>;

struct TaskCompletion {
    std::string reason, doneWhen;
    CompletionProvenance provenance;
};

struct FanoutCompletion {
    std::string doneWhen;
    RunOutcome outcome;
    std::optional<std::string> requestedCommit;
};

enum class WorkflowCompletionOutcome { completed, failed };

struct WorkflowCompletion {
    OutboxSourceRun source;
    std::optional<WorkflowCompletionOutcome> outcome;
};

struct CompletionEntry {
    std::string text;
    std::optional<Id> revisionId;
    std::variant<TaskCompletion, FanoutCompletion, WorkflowCompletion> details;
    const OutboxSourceRun* source() const;
};

struct ToolErrorEntry {
    Id eventId;
    std::string tool, error;
    bool unknownDelivery;
    std::variant<Json, std::string> arguments;
};

struct DecisionEntry {
    Id eventId;
    std::optional<Id> revisionId;
    std::string actionId;
    Decision decision;
    Json evidence, usage;
};

CompletionEntry decodeCompletion(Id, const Json&);
Json encodeCompletion(const CompletionEntry&);
ToolErrorEntry decodeToolError(Id, const Json&);
Json encodeToolError(const ToolErrorEntry&);
DecisionEntry decodeJournalDecision(Id, const Json&);
Json encodeJournalDecision(const DecisionEntry&);
}
