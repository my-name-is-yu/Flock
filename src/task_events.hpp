#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
class TaskRecords;
struct TaskData;

class TaskEvents {
public:
    explicit TaskEvents(Database& database) : database(database) {}

    Id event(TaskData&, std::string_view key, Json data);
    Json currentEvent(const TaskData&) const;
    void consumeEvent(const TaskData&);
    void wakeExternalChecks(TaskRecords&, Seconds now);
private:
    Database& database;
};
}
