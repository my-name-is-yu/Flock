#pragma once

#include "flock/json.hpp"
#include <string_view>

namespace flockv0 {
class Store;
class Database;
class MemoryStorage;
struct Person;

class Memory {
public:
    explicit Memory(Store&);

    Json memory(const Person&, std::string_view bot, std::string_view operation, const Json& args);
private:
    Database& database_;
    MemoryStorage& memory_;
};
}
