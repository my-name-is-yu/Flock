#include "change_feed.hpp"

#include <chrono>

namespace flockv0::web {
using namespace std::chrono_literals;

namespace {
void invalidate(Json& update, const ChangeFeed::BotList&) { update["list"] = true; }
void invalidate(Json& update, const ChangeFeed::BotView& view) { update["bot"] = view.bot; }
void invalidate(Json& update, const ChangeFeed::DirectMessage& dm) {
    update["dm"] = {{"bot",dm.bot},{"peer",dm.peer}};
}
}

ChangeFeed::ChangeFeed(Store& store, Load load)
    : store_(store), load_(std::move(load)), worker_([this] { monitor(); }) {}

ChangeFeed::~ChangeFeed() { stop(); }

std::optional<ChangeFeed::Cursor> ChangeFeed::subscribe(const Scope& view) {
    std::lock_guard lock(mutex_);
    constexpr std::size_t maxSubscribers = 3;
    if (stopping_ || subscribers_ >= maxSubscribers) return std::nullopt;
    Cursor cursor{{Scope{view.person,BotList{}},0},{view,0}};
    if (const auto* dm = std::get_if<DirectMessage>(&view.target))
        cursor.emplace(Scope{view.person,BotView{dm->bot}},0);
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
        std::visit([&](const auto& target) { invalidate(update,target); },scope.target);
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

std::set<ChangeFeed::Scope> ChangeFeed::refresh(const std::vector<Scope>& scopes,
                                                const Store::ConversationChanges& changes) {
    std::set<Scope> failed;
    std::map<Scope,std::optional<Json>> snapshots;
    for (const auto& scope : scopes) {
        auto& snapshot = snapshots[scope];
        try { snapshot = load_(scope); }
        catch (const std::exception&) {}
    }
    std::lock_guard lock(mutex_);
    const auto previousRevision = revision_;
    for (auto& [scope,snapshot] : snapshots) {
        const auto item = watched_.find(scope);
        if (item == watched_.end()) continue;
        auto& watched = item->second;
        if (const auto* view = std::get_if<BotView>(&scope.target); view && changes.messages.contains(view->bot))
            watched.historyPending = true;
        if (!snapshot) {
            // The shared journal cursor can advance while this scope still needs a refresh.
            failed.insert(scope);
            continue;
        }
        const bool historyChanged = std::exchange(watched.historyPending,false) && !snapshot->is_null();
        if (watched.snapshot == snapshot && !historyChanged) continue;
        watched.snapshot = std::move(snapshot);
        watched.revision = ++revision_;
    }
    if (revision_ != previousRevision) changed_.notify_all();
    return failed;
}

void ChangeFeed::monitor() {
    std::unique_lock lock(mutex_);
    std::optional<decltype(store_.changeStamp())> lastStamp;
    std::set<Scope> retry;
    std::optional<Id> journalCursor;
    auto checkProfiles = std::chrono::steady_clock::now();
    while (!stopping_) {
        if (watched_.empty()) {
            journalCursor.reset();
            changed_.wait(lock,[&] { return stopping_ || !watched_.empty(); });
            continue;
        }
        const bool requested = std::exchange(scanRequested_,false);
        const bool profileCheck = std::chrono::steady_clock::now() >= checkProfiles;
        std::vector<Scope> scopes;
        for (const auto& [scope,watched] : watched_) scopes.push_back(scope);
        lock.unlock();
        try {
            const auto stamp = store_.changeStamp();
            if (!requested && !profileCheck && stamp == lastStamp)
                std::erase_if(scopes,[&](const Scope& scope) { return !retry.contains(scope); });
            if (!scopes.empty()) {
                Store::ConversationChanges changes{journalCursor.value_or(0),{}};
                if (!journalCursor || stamp != lastStamp) changes = store_.conversationChangesSince(journalCursor);
                retry = refresh(scopes,changes);
                journalCursor = changes.through;
            }
            lastStamp = stamp;
            if (profileCheck) checkProfiles = std::chrono::steady_clock::now() + 2500ms;
        } catch (const std::exception&) {
            lastStamp.reset();
        }
        lock.lock();
        changed_.wait_for(lock,150ms,[&] { return stopping_ || scanRequested_; });
    }
}

}
