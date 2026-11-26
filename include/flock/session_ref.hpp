#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace flockv0 {
using SessionReadDeadline = std::optional<std::chrono::steady_clock::time_point>;

struct SessionTarget {
    std::string environmentId, threadId;
    std::string gatewayEnvironmentId;
    bool operator==(const SessionTarget&) const = default;
};
struct SessionOrigin {
    SessionTarget target;
    std::string title;
};

struct WorkSession {
    SessionTarget target;
    std::string projectId;
};
}
