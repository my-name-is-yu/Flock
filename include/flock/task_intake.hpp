#pragma once

#include "flock/types.hpp"

namespace flockv0 {
class Store;
class Database;
class TaskOperations;
struct Intake;
struct Person;

class TaskIntake {
public:
    explicit TaskIntake(Store&);

    Id submit(const Intake&, const Person* requester = nullptr);
private:
    Database& database_;
    TaskOperations& operations_;
};
}
