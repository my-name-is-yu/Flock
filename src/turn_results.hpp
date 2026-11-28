#pragma once

#include "tool_call_result.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <cstddef>
#include <map>
#include <utility>

namespace flockv0 {
class Database;

// The results of the tool calls in a conversation turn.
class TurnResults {
public:
    explicit TurnResults(Database& database) : database(database) {}

    void save(Id message, int step, const Json& results);
    void discardUnpersisted(Id message);
    // Results of a running batch, by message and step, that are not in the database yet.
    std::map<std::pair<Id, int>, std::map<std::size_t, ToolCallResult>> unpersisted;
private:
    Database& database;
};
}
