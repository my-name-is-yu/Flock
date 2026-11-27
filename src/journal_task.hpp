#pragma once

#include "flock/task.hpp"
#include "flock/outbox_instruction.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"
#include "flock/run_report.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
struct TaskCompletion {
    std::string reason, doneWhen;
    std::optional<OutboxSourceRun> source;
};

struct FanoutCompletion {
    std::string doneWhen;
    RunOutcome outcome;
    std::optional<std::string> requestedCommit;
};

struct CompletionEntry {
    std::string text;
    std::optional<Id> revisionId;
    std::variant<TaskCompletion, FanoutCompletion> details;
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
