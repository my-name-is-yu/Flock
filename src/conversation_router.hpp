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

struct Target {
    Id taskId, generation, revision, waitId;
    bool operator==(const Target&) const = default;
};

struct RevisionDraft {
    Target target;
    std::string intent, doneWhen;
    bool operator==(const RevisionDraft&) const = default;
};

struct Respond { std::string text; std::optional<Result> source; bool operator==(const Respond&) const = default; };
struct Status { std::string text; std::optional<Target> target; bool operator==(const Status&) const = default; };
struct CreateTask {
    std::string request;
    std::optional<Result> source;
    std::string environment;
    bool operator==(const CreateTask&) const = default;
};
struct AppendInstruction {
    Target target;
    std::string instruction;
    std::optional<Result> source;
    bool operator==(const AppendInstruction&) const = default;
};
struct ReviseTask {
    Target target;
    std::string instruction, intent, doneWhen, environment;
    std::optional<Result> source;
    bool operator==(const ReviseTask&) const = default;
};
struct AnswerWait { Target target; std::string answer; bool operator==(const AnswerWait&) const = default; };
struct CancelTask { Target target; bool operator==(const CancelTask&) const = default; };
struct Clarify {
    std::string question;
    std::vector<Id> candidates;
    std::optional<Result> source = std::nullopt;
    std::string operation{};
    std::optional<RevisionDraft> revisionDraft = std::nullopt;
    bool operator==(const Clarify&) const = default;
};
using Action = std::variant<Respond, Status, CreateTask, AppendInstruction, ReviseTask,
                            AnswerWait, CancelTask, Clarify>;

struct Route {
    std::string segment;
    Action action;
    bool operator==(const Route&) const = default;
};

struct Reply {
    enum class Kind { none, invalid, wait, result, clarification } kind = Kind::none;
    Id id = 0, taskId = 0, waitId = 0;
    std::string originalText, question, operation;
    std::vector<Id> candidates;
    std::optional<Result> result;
    std::optional<RevisionDraft> revisionDraft;
    std::vector<Route> destinationPlan;
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
    Id selection = 0;
    bool moreTasks = false, moreWaits = false, staleWaits = false, multiplePendingClarifications = false;
    const Reply* clarification(Id replyId) const;
    bool operator==(const Context&) const = default;
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
using DecideModel = std::function<Decision(const ModelInput&)>;
Plan decide(std::string_view text, const Context&, const DecideModel&, const BotNotes&, const Json& config,
            const std::function<void(Id resolvedClarificationId)>& resolvedClarification = {});
bool explicitBudgetGrant(std::string_view text);
bool explicitCancellation(std::string_view text);
bool namedTargetMatches(std::string_view text, const Context&, Id target);
bool namesCandidate(std::string_view reply, std::string_view taskName);
}
