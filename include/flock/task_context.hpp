#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flockv0 {
struct DeliveredInstruction {
    std::int64_t messageId;
    int step;
    std::string text;
};

struct TaskRestart {
    std::int64_t taskId;
    std::vector<DeliveredInstruction> deliveredInstructions;
};

enum class TimedTriggerKind { schedule, reminder };

struct TimedTaskTrigger {
    TimedTriggerKind kind;
    std::int64_t targetTime, coveredFrom, occurrenceCount;
    std::string timezone;
    std::optional<std::string> label;
};

struct WebhookTaskTrigger {
    std::string source;
    nlohmann::json event;
};

struct TaskTrigger {
    std::int64_t id, firedAt;
    std::variant<TimedTaskTrigger, WebhookTaskTrigger> details;
};

enum class TaskPeerOutcome { completed, failed };
std::string_view taskPeerOutcomeName(TaskPeerOutcome);
}
