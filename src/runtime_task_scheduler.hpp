#pragma once

#include "flock/core.hpp"

#include <compare>
#include <stop_token>

namespace flockv0 {
class Runtime::TaskScheduler {
public:
    struct Work {
        Id taskId = 0;
        std::optional<std::string> destinationAction;
        auto operator<=>(const Work&) const = default;
    };

    TaskScheduler(Store::Impl& db, T3& t3, TaskUpdates& updates, OutboxDispatch& dispatch, bool requireT3Connection,
                  std::stop_token shutdown);
    void activate(TaskData task);
    std::vector<Work> nextBatch(std::size_t slots, const std::function<bool(const Work&)>& isActive);
private:
    std::vector<Work> eligible();

    bool selectProject(TaskData& task);

    Store::Impl& db;
    T3& t3;
    TaskUpdates& updates;
    OutboxDispatch& dispatch;
    bool requireT3Connection;
    std::stop_token shutdown;
    Work lastScheduled;
};
}
