#pragma once

#include "flock/core.hpp"

#include <utility>
#include <variant>

namespace flockv0::conversation {
struct Task {
    Id id = 0, generation = 0, revision = 0, waitId = 0;
    std::string name, intent, state, doneWhen, waitReason, waitText, fullName;
    std::vector<std::string> waitChoices;
    bool waitAtIntake = false;
    std::string environmentId, projectId;
    bool operator==(const Task&) const = default;
};

struct Result {
    Id taskId = 0, messageId = 0, revision = 0;
    std::string taskName, text, state;
    int option = 0;
    Json provenance = nullptr;
    bool operator==(const Result&) const = default;
};

struct Reply {
    enum class Kind { none, invalid, wait, result, clarification } kind = Kind::none;
    Id id = 0, taskId = 0, waitId = 0;
    std::string originalText, question, operation;
    std::vector<Id> candidates;
    std::optional<Result> result;
    std::vector<std::pair<Id,Id>> waitsAtIntake;
    bool incompleteWaitsAtIntake = true;
    bool operator==(const Reply&) const = default;
};

struct Context {
    std::vector<Task> tasks;
    std::vector<Result> rankedResults;
    Reply reply;
    std::optional<Reply> pendingClarification;
    std::optional<Result> recentSource;
    std::vector<std::string> environments;
    Json destinationIds = Json::object();
    Json history = Json::array(), expiredWaits = Json::array();
    bool moreTasks = false, moreWaits = false, staleWaits = false, multiplePendingClarifications = false;
    const Reply* clarification(Id replyId) const;
    bool operator==(const Context&) const = default;
};

struct Target {
    Id taskId, generation, revision, waitId;
};
struct Respond { std::string text; std::optional<Result> source; };
struct Status { std::string text; std::optional<Target> target; };
struct CreateTask { std::string request; std::optional<Result> source; std::string environment; };
struct AppendInstruction { Target target; std::string instruction; std::optional<Result> source; };
struct ReviseTask {
    Target target;
    std::string instruction, intent, doneWhen, environment;
    std::optional<Result> source;
};
struct AnswerWait { Target target; std::string answer; };
struct CancelTask { Target target; };
struct Clarify {
    std::string question;
    std::vector<Id> candidates;
    std::optional<Result> source = std::nullopt;
    std::string operation{};
};
using Action = std::variant<Respond, Status, CreateTask, AppendInstruction, ReviseTask,
                            AnswerWait, CancelTask, Clarify>;

struct Route {
    std::string segment;
    Action action;
};

struct Resolution {
    enum class Kind { resolved, superseded };
    Id replyId;
    Kind kind;
};
struct Plan {
    std::vector<Route> routes;
    std::optional<Resolution> resolution;
};

std::optional<Plan> shortcut(std::string_view text, const Context&);
Plan decide(std::string_view text, const Context&, Model&, const Json& config);
bool explicitBudgetGrant(std::string_view text);
bool explicitCancellation(std::string_view text);
bool namedTargetMatches(std::string_view text, const Context&, Id target);
bool namesCandidate(std::string_view reply, std::string_view taskName);
}
