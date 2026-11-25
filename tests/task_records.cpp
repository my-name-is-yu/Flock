#include "../src/task_storage.hpp"

#include "flock/record_decode_error.hpp"

#include <array>
#include <iostream>
#include <limits>

using namespace flockv0;
using namespace std::literals;

namespace {
constexpr Id id = 47, generation = 12;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Json stored() {
    return {{"botId", "bot"},
            {"request", "Original request"},
            {"intent", "Revised request"},
            {"environmentId", "net"},
            {"projectId", "project"},
            {"doneWhen", "Provide results"},
            {"config", {{"model", "example"}, {"custom", Json::array({"one", "two"})}}},
            {"activeRun", nullptr},
            {"feedback", "Run is ready"},
            {"question", nullptr}};
}

Json pendingDestination() {
    return {{"environmentId", "remote"}, {"projectId", "other"}, {"gatewayEnvironmentId", "other-gateway"}, {"pinned", true}};
}

void roundTrips() {
    using enum TaskState;
    constexpr std::array states{
        std::pair{queued, "queued"sv},         std::pair{deciding, "deciding"sv},   std::pair{sending, "sending"sv},
        std::pair{running, "running"sv},       std::pair{waiting, "waiting"sv},     std::pair{peer_waiting, "peer_waiting"sv},
        std::pair{cancelling, "cancelling"sv}, std::pair{completed, "completed"sv}, std::pair{cancelled, "cancelled"sv},
    };
    const auto base = stored();
    const auto intake = decodeTask(id, "queued", generation, base.dump());
    check(!intake.gatewayEnvironmentId && !intake.currentRevision && !intake.pendingDestination && encodeTask(intake) == base,
          "Task intake encoded optional fields that were absent");

    auto revised = base;
    revised.update({{"gatewayEnvironmentId", "gateway"}, {"currentRevision", 23}, {"pendingDestination", pendingDestination()}});
    for (const auto& [state, name] : states) {
        const auto task = decodeTask(id, name, generation, revised.dump());
        check(task.id == id && task.generation == generation && task.state == state && taskStateName(task.state) == name,
              "Task columns did not round-trip");
        check(task.botId == "bot" && task.request == "Original request" && task.intent == "Revised request" &&
                  task.environmentId == "net" && task.projectId == "project" && task.doneWhen == "Provide results" &&
                  task.config == base.at("config") && task.gatewayEnvironmentId == "gateway" && task.currentRevision == 23,
              "Task routing and revision fields did not decode");
        check(task.pendingDestination && task.pendingDestination->pinned && task.pendingDestination->route.environmentId == "remote" &&
                  task.pendingDestination->route.projectId == "other" &&
                  task.pendingDestination->route.gatewayEnvironmentId == "other-gateway",
              "Pending destination did not decode");
        check(task.rest == Json{{"activeRun", nullptr}, {"feedback", "Run is ready"}, {"question", nullptr}},
              "Typed fields leaked into TaskData::rest");
        check(encodeTask(task) == revised, "Task payload changed on round-trip");
    }

    auto storedWithColumns = revised;
    storedWithColumns.update({{"id", 10}, {"generation", 9}, {"state", "completed"}, {"revisions", Json::array()}});
    const auto task = decodeTask(id, "queued", generation, storedWithColumns.dump());
    check(task.id == id && task.generation == generation && task.state == queued && encodeTask(task) == revised,
          "Task data overrode a persisted column or retained a projection");

    const TaskView view{.task = task, .revisions = {{23, 2, "Revised request", "Provide results", 33}}};
    const auto projected = taskJson(view);
    check(
        projected.at("id") == id && projected.at("generation") == generation && projected.at("state") == "queued" &&
            projected.at("revisions") ==
                Json::array(
                    {{{"id", 23}, {"number", 2}, {"intent", "Revised request"}, {"doneWhen", "Provide results"}, {"sourceMessageId", 33}}}),
        "Task view changed the wire shape for columns or revisions");
}

void rejects(std::string_view record, std::string_view key, std::string_view issue, std::string_view state = "queued") {
    try {
        decodeTask(id, state, generation, record);
    } catch (const RecordDecodeError& error) {
        const auto expected = "tasks row 47, key " + std::string(key) + ": ";
        const std::string_view message(error.what());
        check(message.starts_with(expected) && message.ends_with(issue), message);
        return;
    }
    throw std::runtime_error("Task decoder accepted invalid " + std::string(key));
}

void decodeErrors() {
    rejects("{", "data", "invalid JSON");
    rejects("[]", "data", "expected an object");
    rejects(stored().dump(), "state", "unknown task state", "invalid");
    for (const auto* key : {"botId", "request", "intent", "environmentId", "projectId", "doneWhen"}) {
        auto invalid = stored();
        invalid.erase(key);
        rejects(invalid.dump(), key, "missing required key");
        invalid[key] = 42;
        rejects(invalid.dump(), key, "expected a string");
    }
    auto invalid = stored();
    invalid["config"] = Json::array();
    rejects(invalid.dump(), "config", "expected an object");
    invalid = stored();
    invalid["gatewayEnvironmentId"] = false;
    rejects(invalid.dump(), "gatewayEnvironmentId", "expected a string");
    invalid = stored();
    invalid["currentRevision"] = 2.5;
    rejects(invalid.dump(), "currentRevision", "expected an integer");
    invalid["currentRevision"] = std::numeric_limits<Json::number_unsigned_t>::max();
    rejects(invalid.dump(), "currentRevision", "integer out of range");
    invalid = stored();
    invalid["pendingDestination"] = nullptr;
    rejects(invalid.dump(), "pendingDestination", "expected an object");
    invalid["pendingDestination"] = pendingDestination();
    invalid["pendingDestination"]["pinned"] = "true";
    rejects(invalid.dump(), "pendingDestination.pinned", "expected a boolean");
    invalid["pendingDestination"] = pendingDestination();
    invalid["pendingDestination"].erase("projectId");
    rejects(invalid.dump(), "pendingDestination.projectId", "missing required key");
}
}

int main() {
    try {
        roundTrips();
        decodeErrors();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
