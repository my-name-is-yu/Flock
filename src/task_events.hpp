#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
struct TaskData;

class TaskEvents {
public:
    explicit TaskEvents(Database& database) : database(database) {}

    Id event(TaskData&, std::string_view key, Json data);
    Json currentEvent(const TaskData&) const;
    void consumeEvent(const TaskData&);
private:
    Database& database;
};
}
