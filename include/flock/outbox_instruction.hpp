#pragma once

#include "flock/run_ref.hpp"

namespace flockv0 {
struct OutboxSourceRun {
    std::string threadId, runId;
    std::optional<std::string> environmentId, gatewayEnvironmentId, link, itemId, status;
    std::optional<RunAssignment> assignment;
};

struct OutboxSource {
    std::int64_t taskId, messageId;
    std::optional<std::int64_t> revisionId{};
    std::optional<std::string> text{}, resultRunId{};
    std::optional<OutboxSourceRun> run{};
};

struct OutboxInstruction {
    std::int64_t messageId;
    int step;
    std::string text;
    std::optional<std::int64_t> revisionId;
    bool conflicting;
    std::optional<OutboxSource> source;
};

}
