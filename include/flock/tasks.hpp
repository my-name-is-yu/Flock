#pragma once

#include "flock/types.hpp"

namespace flockv0 {
class Store;
class Database;
class TaskRecords;
struct TaskView;

class Tasks {
public:
    explicit Tasks(Store&);

    TaskView task(Id taskId) const;
    bool hasTask(Id taskId) const;
private:
    Database& database_;
    const TaskRecords& records_;
};
}
