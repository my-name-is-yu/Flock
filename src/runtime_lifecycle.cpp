#include "runtime_lifecycle.hpp"

#include "flock/participation.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_task_turn.hpp"
#include "storage.hpp"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace flockv0 {
Runtime::DatabaseLock::DatabaseLock(Store::Impl& db, const std::function<Seconds()>& clock) : db(db) {
    const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
    fd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
    }
    try {
        std::lock_guard lock(db.mutex);
        db.policyClock = clock;
    } catch (...) {
        close(fd);
        throw;
    }
}

Runtime::DatabaseLock::~DatabaseLock() {
    {
        std::lock_guard lock(db.mutex);
        db.policyClock = unixTime;
    }
    close(fd);
}

void Runtime::Workers::drain(TaskTurn& taskTurn) {
    requestStop();
    conversations.clear();
    tasks.clear();
    if (channels.valid()) channels.wait();
    if (sessionWake.valid()) sessionWake.wait();
    try {
        taskTurn.persistTaskInferences();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Flock Task inference shutdown: %s\n", error.what());
    }
}

template <class Key> void Runtime::Workers::collect(std::map<Key, std::future<void>>& workers, std::string_view operation) {
    for (auto it = workers.begin(); it != workers.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto result = std::move(it->second);
            it = workers.erase(it);
            maintenance.deferContendedWork(operation, [&] { result.get(); });
        } else
            ++it;
    }
}

void Runtime::Workers::collect(std::future<void>& worker, std::string_view operation) {
    if (worker.valid() && worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        maintenance.deferContendedWork(operation, [&] { worker.get(); });
}

void Runtime::Workers::collectCompleted() {
    collect(conversations, "conversation worker");
    collect(tasks, "task worker");
    collect(channels, "channel worker");
}

void Runtime::Workers::startChannels() {
    if (!channels.valid() && db.channelDeliveryDue())
        channels = std::async(std::launch::async, [db = &db, stop = stopToken()] { db->deliverChannels(stop); });
}

void Runtime::Workers::wakeSessions() {
    db.settleSessionQueueCancellations(t3, stopToken());
    db.wakeSessionMessages(t3, stopToken());
}

void Runtime::Workers::stepSessionWake() {
    collect(sessionWake, "Session wake worker");
    if (!sessionWake.valid()) sessionWake = std::async(std::launch::async, [this] { wakeSessions(); });
}

void Runtime::Workers::startConversations(const std::function<Seconds()>& clock, const std::function<void(Id)>& run) {
    if (conversations.size() >= batchLimit) return;
    for (const auto& [bot, id] : db.pendingConversations(batchLimit, clock())) {
        if (conversations.size() >= batchLimit) break;
        if (!conversations.contains(bot)) {
            conversations.emplace(bot, std::async(std::launch::async, run, id));
        }
    }
}

void Runtime::Workers::startTasks(TaskScheduler& scheduler, const std::function<void(const TaskScheduler::Work&)>& step) {
    for (const auto& work : scheduler.nextBatch(batchLimit - tasks.size(), [this](const auto& job) { return isActive(job); }))
        tasks.emplace(work, std::async(std::launch::async, step, work));
}
}
