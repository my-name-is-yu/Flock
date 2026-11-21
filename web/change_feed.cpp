#include "change_feed.hpp"

#include <chrono>

namespace flockv0::web {
using namespace std::chrono_literals;

ChangeFeed::ChangeFeed(Store& store, Load load)
    : store_(store), load_(std::move(load)), worker_([this] { monitor(); }) {}

ChangeFeed::~ChangeFeed() { stop(); }

std::optional<ChangeFeed::Cursor> ChangeFeed::subscribe(const Scope& view) {
    std::lock_guard lock(mutex_);
    // Each stream holds an HTTP worker. Leave capacity for ordinary requests.
    constexpr std::size_t maxSubscribers = 3;
    if (stopping_ || subscribers_ >= maxSubscribers) return std::nullopt;
    Cursor cursor{{Scope{},0}};
    if (!view.first.empty()) cursor[Scope{view.first,{}}] = 0;
    if (!view.second.empty()) cursor[view] = 0;
    for (auto& [scope,revision] : cursor) {
        auto& watched = watched_[scope];
        ++watched.subscribers;
        revision = watched.revision;
    }
    ++subscribers_;
    scanRequested_ = true;
    changed_.notify_all();
    return cursor;
}

void ChangeFeed::release(const Cursor& cursor) {
    std::lock_guard lock(mutex_);
    for (const auto& [scope,revision] : cursor) {
        auto item = watched_.find(scope);
        if (--item->second.subscribers == 0) watched_.erase(item);
    }
    --subscribers_;
}

std::optional<Json> ChangeFeed::next(Cursor& cursor) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock,1s,[&] {
        if (stopping_) return true;
        for (const auto& [scope,revision] : cursor)
            if (watched_.at(scope).revision != revision) return true;
        return false;
    });
    if (stopping_) return std::nullopt;
    Json update = Json::object();
    for (auto& [scope,revision] : cursor) {
        const auto next = watched_.at(scope).revision;
        if (revision == next) continue;
        revision = next;
        if (scope.first.empty()) update["list"] = true;
        else if (scope.second.empty()) update["bot"] = scope.first;
        else update["dm"] = {{"bot",scope.first},{"peer",scope.second}};
    }
    return update;
}

void ChangeFeed::rescan() {
    std::lock_guard lock(mutex_);
    scanRequested_ = true;
    changed_.notify_all();
}

void ChangeFeed::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        changed_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void ChangeFeed::monitor() {
    std::unique_lock lock(mutex_);
    std::pair<std::int64_t,std::int64_t> lastStamp{};
    auto checkProfiles = std::chrono::steady_clock::now();
    while (!stopping_) {
        if (watched_.empty()) {
            changed_.wait(lock,[&] { return stopping_ || !watched_.empty(); });
            continue;
        }
        const bool requested = std::exchange(scanRequested_,false);
        // Profiles may also be edited on disk. Check them on a slower shared cadence.
        const bool profileCheck = std::chrono::steady_clock::now() >= checkProfiles;
        std::vector<Scope> scopes;
        for (const auto& [scope,watched] : watched_) scopes.push_back(scope);
        lock.unlock();
        try {
            const auto stamp = store_.changeStamp();
            if (requested || profileCheck || stamp != lastStamp) {
                lastStamp = stamp;
                if (profileCheck) checkProfiles = std::chrono::steady_clock::now() + 2500ms;
                std::map<Scope,Json> snapshots;
                for (const auto& scope : scopes) {
                    try { snapshots.emplace(scope,load_(scope)); }
                    catch (const std::exception&) {
                        // An unavailable projection should not block updates for other subscribers.
                        checkProfiles = std::chrono::steady_clock::now() + 150ms;
                    }
                }
                std::lock_guard publish(mutex_);
                bool updated = false;
                for (auto& [scope,snapshot] : snapshots) {
                    const auto item = watched_.find(scope);
                    if (item == watched_.end() || item->second.snapshot == snapshot) continue;
                    item->second.snapshot = std::move(snapshot);
                    item->second.revision = ++revision_;
                    updated = true;
                }
                if (updated) changed_.notify_all();
            }
        } catch (const std::exception&) {
            // A transient read failure leaves the last snapshot intact. Retry on the next scan.
            checkProfiles = std::chrono::steady_clock::now() + 150ms;
        }
        lock.lock();
        changed_.wait_for(lock,150ms,[&] { return stopping_ || scanRequested_; });
    }
}

} // namespace flockv0::web
