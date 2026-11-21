#pragma once

#include "flock/core.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace flockv0::web {

// Empty IDs address the Bot list. A Bot alone addresses its view, and a pair
// addresses its DM. Snapshots stay server-side and use the same projections as GET.
class ChangeFeed {
public:
    using Scope = std::pair<std::string,std::string>;
    using Cursor = std::map<Scope,std::uint64_t>;
    using Load = std::function<Json(const Scope&)>;

    ChangeFeed(Store&, Load);
    ~ChangeFeed();
    std::optional<Cursor> subscribe(const Scope& view);
    void release(const Cursor&);
    std::optional<Json> next(Cursor&);
    void rescan();
    void stop();

private:
    struct Watched {
        std::size_t subscribers = 0;
        std::uint64_t revision = 0;
        std::optional<Json> snapshot;
    };
    void monitor();

    Store& store_;
    Load load_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<Scope,Watched> watched_;
    std::size_t subscribers_ = 0;
    std::uint64_t revision_ = 0;
    bool scanRequested_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace flockv0::web
