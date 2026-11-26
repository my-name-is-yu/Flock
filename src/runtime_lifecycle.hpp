#pragma once

#include "runtime_components.hpp"
#include "runtime_task_scheduler.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>

namespace flockv0 {
class ChannelStorage;
class Database;
class PolicyApprovals;

class Runtime::Components::DatabaseLock {
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

class Runtime::Components::Workers {
public:
    static constexpr unsigned batchLimit = 4;

    Workers(Store::Impl& db, ChannelStorage& channelStorage, T3& t3, Maintenance& maintenance)
        : db(db), channelStorage(channelStorage), t3(t3), maintenance(maintenance) {}
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    void requestStop() { shutdown.request_stop(); }
    std::stop_token stopToken() const { return shutdown.get_token(); }
    void drain(TaskTurn& taskTurn);
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

    Store::Impl& db;
    ChannelStorage& channelStorage;
    T3& t3;
    Maintenance& maintenance;
    std::stop_source shutdown;
    std::map<std::string, std::future<void>> conversations;
    std::map<TaskScheduler::Work, std::future<void>> tasks;
    std::future<void> channels, sessionWake;
};
}
