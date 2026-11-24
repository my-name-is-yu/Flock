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

std::shared_ptr<const ChangeFeed::Projection> ChangeFeed::cachedBotView(const Person& person, const std::string& bot,
                                                                      const Json& descriptor) {
    std::lock_guard lock(mutex_);
    const auto item = watched_.find({person,BotView{bot}});
    if (item == watched_.end()) return {};
    const auto& snapshot = item->second.snapshot;
    if (!snapshot || !snapshot->botVersion || snapshot->botVersion->descriptor != descriptor) return {};
    return snapshot;
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

void ChangeFeed::refresh(const std::vector<Scope>& scopes, const Store::ConversationChanges& changes) {
    std::map<Scope,std::shared_ptr<const Projection>> snapshots;
    for (const auto& scope : scopes) {
        auto& snapshot = snapshots[scope];
        try { snapshot = std::make_shared<const Projection>(load_(scope)); }
        catch (const std::exception&) {}
    }
    std::lock_guard lock(mutex_);
    const auto previousRevision = revision_;
    for (auto& [scope,snapshot] : snapshots) {
        const auto item = watched_.find(scope);
        if (item == watched_.end()) continue;
        auto& watched = item->second;
        if (const auto* view = std::get_if<BotView>(&scope.target); view && changes.messages.contains(view->bot))
            watched.pending = Refresh::History;
        if (!snapshot) {
            if (watched.pending == Refresh::None) watched.pending = Refresh::Projection;
            continue;
        }
        const bool historyChanged = std::exchange(watched.pending,Refresh::None) == Refresh::History && !snapshot->data.is_null();
        const bool projectionChanged = !watched.snapshot || watched.snapshot->data != snapshot->data;
        watched.snapshot = std::move(snapshot);
        if (projectionChanged || historyChanged) watched.revision = ++revision_;
    }
    if (revision_ != previousRevision) changed_.notify_all();
}

void ChangeFeed::monitor() {
    std::unique_lock lock(mutex_);
    std::optional<decltype(store_.changeStamp())> lastStamp;
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
        std::vector<Scope> pending;
        for (const auto& [scope,watched] : watched_) {
            scopes.push_back(scope);
            if (watched.pending != Refresh::None) pending.push_back(scope);
        }
        lock.unlock();
        try {
            const auto stamp = store_.changeStamp();
            if (!requested && !profileCheck && stamp == lastStamp)
                scopes = std::move(pending);
            if (!scopes.empty()) {
                Store::ConversationChanges changes{journalCursor.value_or(0),{}};
                if (!journalCursor || stamp != lastStamp) changes = store_.conversationChangesSince(journalCursor);
                refresh(scopes,changes);
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
