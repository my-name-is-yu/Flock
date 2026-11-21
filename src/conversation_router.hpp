#pragma once

#include "flock/core.hpp"

namespace flockv0::conversation {
enum class Kind {
    respond, status, createTask, appendInstruction, reviseTask, answerWait, grantOne, cancelTask, clarify
};

struct Task {
    Id id = 0, generation = 0, revision = 0, waitId = 0;
    std::string name, intent, state, doneWhen, waitReason, waitText;
    std::vector<std::string> waitChoices;
};

struct Result {
    Id taskId = 0, messageId = 0, revision = 0;
    std::string taskName, text, state;
    Json provenance = nullptr;
};

struct Reply {
    enum class Kind { none, invalid, wait, result, clarification } kind = Kind::none;
    Id id = 0, taskId = 0, waitId = 0;
    std::string originalText;
    std::vector<Id> candidates;
    std::optional<Result> result;
};

struct Context {
    std::vector<Task> tasks;
    std::vector<Result> results; // Newest first, with source identity and bounded text.
    Reply reply;
    std::optional<Result> recentSource;
    std::vector<std::string> environments;
};

struct Route {
    Kind kind = Kind::clarify;
    Id taskId = 0, generation = 0, revision = 0, waitId = 0;
    Id clarification = 0;
    std::string segmentText, text, intent, doneWhen, environment;
    std::vector<Id> candidates;
    std::optional<Result> source;
    bool conflicting = false, superseded = false;
};

std::vector<Route> route(std::string_view text, const Context&);
bool usesResults(std::string_view text);
bool namesCandidate(std::string_view reply, std::string_view taskName);
} // namespace flockv0::conversation
