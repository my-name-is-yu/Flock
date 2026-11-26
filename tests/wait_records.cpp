#include "../src/wait_storage.hpp"
#include "flock/record_decode_error.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
    const Wait wait{details, "Paused", id, 21, 32, "flock-example", std::move(run)};
    check(to_json(wait) == expected, "Wait encoded an unexpected public shape");
    auto storedData = expected;
    storedData.erase("reason");
    const auto reason = storedWaitReason(wait);
    check(reason == (std::holds_alternative<ProjectQuestionWait>(details) ? "project_question" : waitReasonName(wait.reason())) &&
              encodeWaitData(wait) == storedData,
          "Wait encoded an unexpected SQLite shape");
    const auto decoded = decodeWait(id, reason, storedData.dump());
    check(decoded.id == id && decoded.details.index() == details.index() && decoded.reason() == wait.reason(),
          "decoded wait lost its row ID or reason variant");
    check(!decoded.messageId && to_json(decoded) == expected, "round-trip added presentation fields or changed wait data");
    auto entry = expected;
    entry.update({{"id", id}, {"messageId", 32}});
    const auto journal = decodeWaitingEntry(101, entry);
    check(journal.id == id && journal.details.index() == details.index() && journal.messageId == 32 && to_json(journal) == expected,
          "Wait decoded an unexpected journal shape");
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

void rejectsRecord(const auto& decode, std::string context, std::string_view key, std::string_view issue) {
    try {
        decode();
    } catch (const RecordDecodeError& error) {
        const std::string_view message(error.what());
        check(message.find(context + ", key " + std::string(key) + ":") != message.npos && message.find(issue) != message.npos,
              "decode failure lost its wait row, key or cause");
        return;
    }
    throw std::runtime_error("invalid Wait decoded");
}

void rejects(std::string_view reason, const Json& record, std::string_view key, std::string_view issue) {
    rejectsRecord([&] { decodeWait(47, reason, record.dump()); }, "waits row 47", key, issue);
}

void decodeErrors() {
    auto payload = stored("settings");
    payload.erase("reason");
    auto missing = payload;
    missing.erase("eventId");
    rejects("settings", missing, "eventId", "missing required key");
    for (const auto* reason : {"unsupported", "budget", "model_budget", "conversation_model_budget", "deadline"})
        rejects(reason, payload, "reason", "unknown wait reason");
    rejects("settings", stored("settings"), "reason", "unknown key");
    auto badCode = payload;
    badCode["failureCode"] = "other";
    rejects("model_failure", badCode, "failureCode", "unknown model failure code");
    auto nested = payload;
    nested["run"] = {{"threadId", "thread"}, {"runId", "run"}, {"link", 5}};
    rejects("settings", nested, "run.link", "expected a string");
    auto modelQuestion = payload;
    modelQuestion.update({{"choices", Json::array()}, {"questionId", ""}, {"decisionReason", ""}});
    rejects("project_question", modelQuestion, "projectChoices", "missing required key");
    modelQuestion["projectChoices"] = Json::object();
    rejects("user_question", modelQuestion, "projectChoices", "unknown key");

    auto entry = stored("settings");
    entry["id"] = 47;
    entry["choices"] = Json::array();
    const auto decodeEntry = [&] { decodeWaitingEntry(101, entry); };
    rejectsRecord(decodeEntry, "journal row 101", "choices", "unknown key");
    entry.erase("choices");
    entry["id"] = "47";
    rejectsRecord(decodeEntry, "journal row 101", "id", "expected an integer");
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
