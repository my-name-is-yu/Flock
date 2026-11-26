#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <string_view>

namespace flockv0 {
class Store;
class Database;
class WorkflowStorage;

class Workflows {
public:
    explicit Workflows(Store&);

    Json workflows(std::string_view bot, Id before = 0, unsigned limit = 20) const;
private:
    Database& database_;
    const WorkflowStorage& workflows_;
};
}
