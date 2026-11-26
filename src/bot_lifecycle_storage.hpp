#pragma once

#include <string_view>

namespace flockv0 {
class Database;

class BotLifecycleStorage {
public:
    explicit BotLifecycleStorage(Database& db) : db_(db) {}
    bool botDeleted(std::string_view bot) const;
    bool botPaused(std::string_view bot) const;
private:
    Database& db_;
};
}
