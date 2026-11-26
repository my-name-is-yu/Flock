#pragma once

#include "flock/types.hpp"

#include <functional>
#include <string_view>

namespace flockv0 {
struct Person;
class Store;

struct BotBusy : Conflict {
    BotBusy() : Conflict("Bot has unfinished work") {}
};

inline constexpr char botPausedMessage[] = "This Bot is paused.";

struct BotPaused : Conflict {
    BotPaused() : Conflict(botPausedMessage) {}
};

class BotLifecycle {
public:
    explicit BotLifecycle(Store& store) : store_(store) {}

    bool botDeleted(std::string_view bot) const;
    void deleteBot(const Person&, std::string_view bot);
    bool botPaused(std::string_view bot) const;
    void setBotPaused(const Person&, std::string_view bot, bool paused, const std::function<Seconds()>& clock = unixTime);
private:
    Store& store_;
};
}
