#pragma once

#include <string>

namespace flockv0 {
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
