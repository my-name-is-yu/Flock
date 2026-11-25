#pragma once

#include "flock/core.hpp"

#include <compare>

namespace flockv0 {
class Runtime::TaskScheduler {
public:
    struct Work {
        Id taskId = 0;
        std::optional<std::string> destinationAction;
        auto operator<=>(const Work&) const = default;
    };

    explicit TaskScheduler(Store::Impl&);
    std::vector<Work> nextBatch(std::size_t slots, const std::function<bool(const Work&)>& isActive);
private:
    std::vector<Work> eligible();

    Store::Impl& db;
    Work lastScheduled;
};
}
