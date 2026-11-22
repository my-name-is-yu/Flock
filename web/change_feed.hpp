#pragma once

#include "flock/access.hpp"

#include <compare>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <variant>

namespace flockv0::web {

class ChangeFeed {
public:
    struct BotList {
        auto operator<=>(const BotList&) const = default;
    };
    struct BotView {
        std::string bot;
        auto operator<=>(const BotView&) const = default;
    };
    struct DirectMessage {
        std::string bot, peer;
        auto operator<=>(const DirectMessage&) const = default;
    };
    using Target = std::variant<BotList,BotView,DirectMessage>;
    struct Scope {
        Person person;
        Target target;
        auto operator<=>(const Scope&) const = default;
    };
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
    std::set<Scope> refresh(const std::vector<Scope>&, const Store::ConversationChanges&);

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

}
