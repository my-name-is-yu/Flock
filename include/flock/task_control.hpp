#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <string_view>

namespace flockv0 {
class Store;
class Database;
class TaskOperations;
struct Person;
struct TaskTarget;

class TaskControl {
public:
    explicit TaskControl(Store&);

    void operate(const Person&, const TaskTarget&, std::string_view operation, const Json& data = Json::object());
private:
    Database& database_;
    TaskOperations& operations_;
};
}
