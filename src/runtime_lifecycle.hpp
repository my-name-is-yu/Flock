#pragma once

#include "runtime_components.hpp"
#include "runtime_task_scheduler.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>

namespace flockv0 {
class ConversationTurns;
class SessionMessageStorage;
class SessionParticipationStorage;

class ChannelStorage;
class Database;
class PolicyApprovals;
class T3;

class runtime::DatabaseLock {
public:
    DatabaseLock(Database& db, const std::filesystem::path& path, PolicyApprovals& approvals, const std::function<Seconds()>& clock);
    ~DatabaseLock();
    DatabaseLock(const DatabaseLock&) = delete;
    DatabaseLock& operator=(const DatabaseLock&) = delete;
private:
    Database& db;
    PolicyApprovals& approvals;
    int fd;
};

class runtime::Workers {
public:
    static constexpr unsigned batchLimit = 4;

    Workers(ConversationTurns& conversationTurns, SessionMessageStorage& sessionMessageStorage,
            SessionParticipationStorage& sessionParticipationStorage, ChannelStorage& channelStorage, T3& t3, Maintenance& maintenance)
        : conversationTurns(conversationTurns), sessionMessageStorage(sessionMessageStorage),
          sessionParticipationStorage(sessionParticipationStorage), channelStorage(channelStorage), t3(t3), maintenance(maintenance) {}
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    void requestStop() { shutdown.request_stop(); }
    std::stop_token stopToken() const { return shutdown.get_token(); }
    void drain();
    bool isActive(const TaskScheduler::Work& work) const { return tasks.contains(work); }

    void collectCompleted();
    void startChannels();
    void wakeSessions();
    void stepSessionWake();
    void startConversations(const std::function<Seconds()>& clock, const std::function<void(Id)>& run);
    void startTasks(TaskScheduler& scheduler, const std::function<void(const TaskScheduler::Work&)>& step);
private:
    template <class Key> void collect(std::map<Key, std::future<void>>& workers, std::string_view operation);
    void collect(std::future<void>& worker, std::string_view operation);

    ConversationTurns& conversationTurns;
    SessionMessageStorage& sessionMessageStorage;
    SessionParticipationStorage& sessionParticipationStorage;
    ChannelStorage& channelStorage;
    T3& t3;
    Maintenance& maintenance;
    std::stop_source shutdown;
    std::map<std::string, std::future<void>> conversations;
    std::map<TaskScheduler::Work, std::future<void>> tasks;
    std::future<void> channels, sessionWake;
};
}
