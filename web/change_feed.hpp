#pragma once

#include "flock/notifications.hpp"
#include "flock/conversations.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace flockv0 {
class Store;
}

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
    using Target = std::variant<BotList, BotView>;
    struct Scope {
        Person person;
        Target target;
        auto operator<=>(const Scope&) const = default;
    };
    using Cursor = std::map<Scope, std::uint64_t>;
    struct BotVersion {
        Notifications::BotViewStamp stamp;
        Json descriptor;
    };
    struct Projection {
        Projection(Json data, std::optional<BotVersion> version = std::nullopt) : data(std::move(data)), botVersion(std::move(version)) {}
        Json data;
        std::optional<BotVersion> botVersion;
    };
    using Load = std::function<Projection(const Scope&)>;

    ChangeFeed(Store&, Load);
    ~ChangeFeed();
    std::optional<Cursor> subscribe(const Scope& view);
    void release(const Cursor&);
    std::optional<Json> next(Cursor&);
    std::shared_ptr<const Projection> cachedBotView(const Person&, const std::string& bot, const Json& descriptor);
    void rescan();
    void stop();
private:
    enum class Refresh { None, Projection, History };
    struct Watched {
        std::size_t subscribers = 0;
        std::uint64_t revision = 0;
        std::shared_ptr<const Projection> snapshot;
        Refresh pending = Refresh::None;
    };
    void monitor();
    void refresh(const std::vector<Scope>&, const Conversations::ConversationChanges&);

    Store& store_;
    Load load_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<Scope, Watched> watched_;
    std::size_t subscribers_ = 0;
    std::uint64_t revision_ = 0;
    bool scanRequested_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}
