#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace flockv0 {
struct StoreOptions;

// Separate CLI and Web connections may use the same database.
class Store {
public:
    explicit Store(std::filesystem::path database);
    Store(std::filesystem::path database, StoreOptions);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // A change hint for readers, covering writes on this connection and other connections.
    std::pair<std::int64_t, std::int64_t> changeStamp() const;
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class BotLifecycle;
    friend class Channels;
    friend class LocalMachines;
    friend class WebIdentity;
    friend class Workflows;
    friend class Notifications;
    friend class Tasks;
    friend class TaskIntake;
    friend class TaskControl;
    friend class TaskList;
    friend class TaskHistory;
    friend class Memory;
    friend class Automations;
    friend class Inbox;
    friend class GitEvents;
    friend class Conversations;
    friend class ConversationIntake;
    friend class WebConversationIntake;
    friend class Runtime;
    friend class StoreAccess;
    friend class T3Connections;
    friend class ExternalMcp;
    friend class SessionParticipation;
    friend class Database;
};
}
