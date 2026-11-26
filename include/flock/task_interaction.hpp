#pragma once

#include "flock/decision.hpp"
#include "flock/wait.hpp"

namespace flockv0 {
struct QuestionOption {
    std::string label, value, description{};
};

struct QuestionField {
    std::string id, prompt{}, header{};
    bool required = true, multiple = false, freeform = true;
    std::vector<QuestionOption> options{};
    std::optional<std::string> initialAnswer{};
};

struct Question {
    std::string requestId;
    std::vector<QuestionField> fields{};
};

struct TaskDecision : Decision {
    nlohmann::json usage = nlohmann::json::object();
    std::int64_t attemptId = 0;
    std::string rejection{};
    std::optional<nlohmann::json> setup{};
};

struct DecisionCheckpoint {
    std::int64_t generation, eventId;
    std::optional<std::int64_t> revisionId;
    TaskDecision decision;
};

struct QuestionTarget {
    std::optional<std::int64_t> waitId;
    std::int64_t eventId;
    std::optional<WaitRun> run;
    std::string requestId;

    bool operator==(const QuestionTarget&) const = default;
};

struct TaskQuestion {
    std::vector<QuestionField> fields;
    QuestionTarget target;
    std::optional<std::int64_t> suspendedWaitId{};
    std::optional<DecisionCheckpoint> suspendedDecision{};
};

struct RecordedTaskBlocker {
    std::string reason;
    std::optional<std::int64_t> waitId;
};
}
