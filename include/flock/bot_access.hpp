#pragma once

#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <functional>
#include <string_view>

namespace flockv0 {
class Store;
struct Person;

class BotAccess {
public:
    explicit BotAccess(Store& store) : store_(store) {}

    Json createOwnedBot(const Person&, const std::function<Json(const PublishOwnedBot&)>&);
    void rememberProjectAssignment(const Person&, std::string_view bot, std::string_view environment, std::string_view project);
    Json recentProjectAssignment(const Person&) const;
    void setOwner(const Person& creator, std::string_view bot);
    void setMember(const Person&, std::string_view bot, std::string_view person, bool add);
    Json botSettings(const Person&, std::string_view bot) const;
    Id applyBotChange(std::string_view bot, const Person& creator, std::string_view target, std::string_view name, Id message, int step,
                      const ApplyBotChange&, const ReconcileBotCreation* onExhausted = nullptr);
private:
    Store& store_;
};
}
