#pragma once

#include "flock/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
class Database;
struct BotMessageEntry;
struct TaskData;

struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
    bool allowsReplan(Id task) const { return recipient == task && kind == "work" && phase == "working"; }
};

class PeerStorage {
public:
    explicit PeerStorage(Database& db) : db_(db) {}
    std::optional<PeerExchange> peerExchange(Id) const;
    bool peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const;
    bool recipientRouteWouldCycle(Id task, std::string_view environment, std::string_view project) const;
    bool replanReady(const TaskData& task) const;
    void peerMessage(const PeerExchange&, const BotMessageEntry&, std::optional<Id> forwardedSource);
private:
    Database& db_;
};
}
