#pragma once

#include "flock/json.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
struct TaskData;

class TaskEvents {
public:
    TaskEvents(Database& database, Store::Impl& remaining) : database(database), remaining(remaining) {}

    Id event(TaskData&, std::string_view key, Json data);
    Json currentEvent(const TaskData&) const;
    void consumeEvent(const TaskData&);
private:
    Database& database;
    Store::Impl& remaining;
};
}
