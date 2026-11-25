#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace flockv0 {
struct RunRef {
    std::string threadId, runId, link;
    std::string gatewayEnvironmentId;
    bool operator==(const RunRef&) const = default;
};

struct RunAssignment {
    std::string ownerBotId, projectId;
    std::optional<std::int64_t> revisionId;
    bool operator==(const RunAssignment&) const = default;
};
}
