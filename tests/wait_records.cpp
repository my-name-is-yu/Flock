#include "../src/wait_storage.hpp"
#include "flock/record_decode_error.hpp"

#include <iostream>

using namespace flockv0;
namespace {
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Json stored(std::string_view reason) {
    return {{"reason", reason}, {"eventId", 21}, {"text", "Paused"}, {"actionId", "flock-example"}, {"run", nullptr}};
}

void roundTrip(const WaitDetails& details, Json expected, std::optional<WaitRun> run = {}) {
    constexpr Id id = 47;
    const Wait wait{details, "Paused", id, 21, 32, 3, "flock-example", std::move(run)};
    const auto encoded = to_json(wait);
    check(encoded == expected, "Wait encoded an unexpected SQLite shape: " + encoded.dump());
    const auto decoded = from_json(id, encoded.dump());
    check(decoded.id == id && decoded.details.index() == details.index() && decoded.reason() == wait.reason(),
          "decoded wait lost its row ID or reason variant");
    check(!decoded.messageId && !decoded.allowance && to_json(decoded) == expected,
          "round-trip added presentation fields or changed wait data");
}

void roundTrips() {
    auto question = stored("user_question");
    question.update({{"choices", Json::array({"Yes", "No"})}, {"decisionReason", "Need approval"}, {"questionId", "t3-question"}});
    question["run"] = {
        {"threadId", "thread"},           {"runId", "run"},      {"link", "https://t3.test/thread"}, {"gatewayEnvironmentId", "gateway"},
        {"environmentId", "environment"}, {"ownerBotId", "bot"}, {"projectId", "project"},           {"revisionId", nullptr}};
    roundTrip(ModelQuestionWait{{"Yes", "No"}, "Need approval", "t3-question"}, question,
              WaitRun{{"thread", "run", "https://t3.test/thread", "gateway"}, "environment", RunAssignment{"bot", "project", {}}});

    auto project = stored("user_question");
    project.update({{"choices", Json::array({"Mobile", "Web"})}, {"projectChoices", {{"Mobile", "app"}, {"Web", "site"}}}});
    roundTrip(ProjectQuestionWait{{"Mobile", "Web"}, {{"Mobile", "app"}, {"Web", "site"}}}, project);

    auto budget = stored("budget");
    budget.update({{"attempted", "follow_up"},
                   {"decisionReason", "Run more checks"},
                   {"usage",
                    {{"input_tokens", 10},
                     {"output_tokens", 4},
                     {"total_tokens", 14},
                     {"input_tokens_details", {{"cached_tokens", 2}}},
                     {"wallMs", 1800}}}});
    roundTrip(WorkBudgetWait{WorkBudgetTool::follow_up, "Run more checks", {10, 4, 14, 2, 1800}}, budget);
    roundTrip(SimpleWaitReason::model_budget, stored("model_budget"));

    roundTrip(DeadlineWait{}, stored("deadline"));
    auto deadline = stored("deadline");
    deadline.update({{"deadline", 200},
                     {"runActionId", "verify-action"},
                     {"run", {{"threadId", "verify-thread"}, {"runId", ""}, {"link", ""}, {"environmentId", "build"}}}});
    roundTrip(DeadlineWait{VerificationDeadline{200, "verify-action"}}, deadline, WaitRun{{"verify-thread", "", "", ""}, "build"});

    roundTrip(ModelFailureWait{}, stored("model_failure"));
    auto failure = stored("model_failure");
    failure["failureCode"] = "unsupported_reasoning_effort";
    roundTrip(ModelFailureWait{WaitFailureCode::unsupported_reasoning_effort}, failure);

    roundTrip(SimpleWaitReason::bot_notes_unavailable, stored("bot_notes_unavailable"));
    roundTrip(SimpleWaitReason::t3_input, stored("t3_input"));
    roundTrip(SimpleWaitReason::auth, stored("auth"));
    roundTrip(SimpleWaitReason::settings, stored("settings"));
    roundTrip(SimpleWaitReason::unknown_delivery, stored("unknown_delivery"));
    auto policy = stored("policy_approval");
    policy["approvalId"] = 26;
    roundTrip(PolicyApprovalWait{26}, policy);
}

void rejects(const Json& record, std::string_view key, std::string_view issue) {
    try {
        from_json(47, record.dump());
    } catch (const RecordDecodeError& error) {
        const std::string_view message(error.what());
        check(message.find("waits row 47, key " + std::string(key) + ":") != std::string_view::npos && message.find(issue) != message.npos,
              "decode failure lost its wait row, key or cause");
        return;
    }
    throw std::runtime_error("invalid Wait decoded");
}

void decodeErrors() {
    auto missing = stored("settings");
    missing.erase("eventId");
    rejects(missing, "eventId", "missing required key");
    auto wrong = stored("deadline");
    wrong.update({{"deadline", 1.5}, {"runActionId", "verify"}});
    rejects(wrong, "deadline", "expected an integer");
    rejects(stored("unsupported"), "reason", "unknown wait reason");
    auto badCode = stored("model_failure");
    badCode["failureCode"] = "other";
    rejects(badCode, "failureCode", "unknown model failure code");
    auto nested = stored("settings");
    nested["run"] = {{"threadId", "thread"}, {"runId", "run"}, {"link", 5}};
    rejects(nested, "run.link", "expected a string");
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
