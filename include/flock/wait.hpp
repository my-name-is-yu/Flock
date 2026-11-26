#pragma once

#include "flock/run_ref.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flockv0 {
enum class WaitReason { user_question, model_failure, bot_notes_unavailable, t3_input, auth, settings, unknown_delivery, policy_approval };

enum class SimpleWaitReason { bot_notes_unavailable, t3_input, auth, settings, unknown_delivery };
enum class WaitFailureCode { unsupported_reasoning_effort };

std::string_view waitReasonName(WaitReason);
WaitReason waitReason(SimpleWaitReason);

struct ModelQuestionWait {
    std::vector<std::string> choices;
    std::string decisionReason;
    std::string questionId;
};
struct ProjectQuestionWait {
    std::vector<std::string> choices;
    std::map<std::string, std::string> projects;
};
struct ModelFailureWait {
    std::optional<WaitFailureCode> code;
};
struct PolicyApprovalWait {
    std::int64_t approvalId;
};
using WaitDetails = std::variant<SimpleWaitReason, ModelQuestionWait, ProjectQuestionWait, ModelFailureWait, PolicyApprovalWait>;

struct WaitRun {
    RunRef run;
    std::optional<std::string> environmentId{};
    std::optional<RunAssignment> assignment{};
    bool operator==(const WaitRun&) const = default;
};

struct Wait {
    WaitDetails details;
    std::string text;
    std::int64_t id = 0, eventId = 0;
    std::optional<std::int64_t> messageId{};
    std::optional<std::string> actionId{};
    std::optional<WaitRun> run{};

    WaitReason reason() const;
    std::span<const std::string> choices() const;
};
}
