#include "../src/outbox_storage.hpp"

#include "flock/record_decode_error.hpp"

#include <array>
#include <iostream>

using namespace std::literals;
using namespace flockv0;

namespace {
constexpr auto id = "outbox-test-action"sv;
constexpr auto authGeneration = "auth-generation"sv;

void check(bool passed, std::string_view message) {
    if (!passed) throw std::runtime_error(std::string(message));
}

Json payload() {
    const Json source = {{"taskId", 11},
                         {"messageId", 12},
                         {"revisionId", 13},
                         {"run", {{"threadId", "source-thread"}, {"runId", "source-run"}, {"itemId", "source-item"}}}};
    auto instructionSource = source;
    instructionSource["text"] = "Earlier result";
    return {{"tool", "follow_up"},
            {"environmentId", "environment"},
            {"projectId", "project"},
            {"target", {{"threadId", "thread"}, {"runId", "run"}, {"link", "link"}, {"gatewayEnvironmentId", "gateway"}}},
            {"gatewayEnvironmentId", "gateway"},
            {"instruction", "Continue the work"},
            {"questionId", ""},
            {"answers", Json::object()},
            {"revisionId", 13},
            {"instructions", Json::array({{{"messageId", 14},
                                           {"step", 2},
                                           {"text", "Earlier direction"},
                                           {"revisionId", nullptr},
                                           {"conflicting", false},
                                           {"source", instructionSource}}})},
            {"contextIds", {{"answerEventId", 15}, {"peerMessageId", "peer-message"}, {"sourceMessageId", 17}}},
            {"sourceResultRefs", Json::array({source})},
            {"chargesBudget", true},
            {"destinationAction", true},
            {"expectedCommit", "abc123"},
            {"markedCommitChecks", true},
            {"candidate", {{"threadId", "candidate-thread"}, {"runId", "candidate-run"}, {"link", "candidate-link"}}},
            {"candidateQuestionId", "candidate-question"},
            {"launchReceiptThreadId", "launch-receipt-thread"},
            {"sessionParticipationId", 18},
            {"sessionMemory", {{"version", 19}, {"section", "Session notes"}, {"excerptBytes", 21}}},
            {"stopRequested", true},
            {"stopOperation", "stop-action"},
            {"stopRunId", "stop-run"},
            {"stopThreadId", "stop-thread"},
            {"stopPolicy",
             {{"tool", "cancel_task"}, {"args", {{"taskId", 11}, {"destination", "left"}}}, {"generation", 22}, {"revisionId", 13}}},
            {"stopPolicyOrigin", {{"scope", "task/11"}, {"requester", "user"}, {"anchor", 24}, {"turn", 0}, {"background", false}}}};
}

void roundTripPhases() {
    using enum OutboxPhase;
    constexpr std::array phases{
        std::pair{ready, "ready"sv},   std::pair{started, "started"sv},       std::pair{accepted, "accepted"sv},
        std::pair{closed, "closed"sv}, std::pair{superseded, "superseded"sv},
    };
    const auto stored = payload();
    for (const auto& [phase, name] : phases) {
        const auto row = from_json(id, name, authGeneration, stored.dump());
        check(row.phase == phase && outboxPhaseName(row.phase) == name, "Outbox phase did not round-trip");
        check(row.command.actionId == id && row.authGeneration == authGeneration, "Outbox lost its ID or auth generation");
        check(row.command.tool == CommandTool::follow_up && row.command.target.runId == "run", "Outbox lost its follow-up target");
        check(row.stop && row.stop->run && row.stop->run->runId == "stop-run" && row.stop->approval &&
                  row.stop->approval->scope == "task/11",
              "Outbox lost its stop request");
        const auto encoded = to_json(row);
        check(encoded == stored, "Outbox payload changed on round-trip. Diff " + Json::diff(stored, encoded).dump());
    }
}

void roundTripDeferredAndAnswerCommands() {
    Outbox deferred;
    deferred.command.tool = CommandTool::delegate;
    deferred.command.environmentId = "environment";
    deferred.command.projectId = "project";
    deferred.instructionBase = "Start the work";
    auto stored = to_json(deferred);
    auto row = from_json(id, "ready", "", stored.dump());
    check(stored.at("target").is_null() && stored.at("revisionId").is_null() && !stored.contains("instruction"),
          "A deferred launch saved a concrete target, revision or instruction");
    check(row.instructionBase == deferred.instructionBase && !row.revisionId && row.command.target.threadId.empty() &&
              !row.command.candidate && !row.stop && to_json(row) == stored,
          "A deferred launch did not round-trip");

    deferred.command.tool = CommandTool::start_in_session;
    deferred.command.target.threadId = "selected";
    stored = to_json(deferred);
    row = from_json(id, "ready", "", stored.dump());
    check(row.command.tool == CommandTool::start_in_session && row.command.target.threadId == "selected" && to_json(row) == stored,
          "A deferred Session start did not round-trip");

    Outbox reply;
    reply.command.tool = CommandTool::answer_session_question;
    reply.command.target = {"thread", "run", "link", {}};
    reply.command.questionId = "question";
    reply.command.answers = {{"branch", "main"}, {"checks", Json::array({"unit", "integration"})}};
    stored = to_json(reply);
    row = from_json(id, "closed", authGeneration, stored.dump());
    check(row.command.tool == CommandTool::answer_session_question && row.command.questionId == "question" &&
              row.command.answers == reply.command.answers && to_json(row) == stored,
          "A Session answer did not round-trip");
}

void expectDecodeError(std::string_view stored, std::string_view key, std::string_view phase = "ready") {
    try {
        from_json(id, phase, authGeneration, stored);
    } catch (const RecordDecodeError& error) {
        const auto expected = "outbox row " + std::string(id) + ", key " + std::string(key) + ": ";
        check(std::string_view(error.what()).starts_with(expected), error.what());
        return;
    }
    throw std::runtime_error("Outbox decode accepted invalid " + std::string(key));
}

void rejectsInvalidRows() {
    const auto stored = payload();
    expectDecodeError("{", "data");
    expectDecodeError(stored.dump(), "phase", "dispatched");
    auto invalid = stored;
    invalid.erase("projectId");
    expectDecodeError(invalid.dump(), "projectId");
    invalid = stored;
    invalid["revisionId"] = 2.5;
    expectDecodeError(invalid.dump(), "revisionId");
    invalid = stored;
    invalid["tool"] = "send";
    expectDecodeError(invalid.dump(), "tool");
    invalid["tool"] = "message_session";
    expectDecodeError(invalid.dump(), "tool");
    invalid = stored;
    invalid.erase("instruction");
    expectDecodeError(invalid.dump(), "instruction");
    invalid = stored;
    invalid["instructions"][0]["step"] = "2";
    expectDecodeError(invalid.dump(), "instructions.0.step");
    invalid = stored;
    invalid["sessionMemory"]["excerptBytes"] = -1;
    expectDecodeError(invalid.dump(), "sessionMemory.excerptBytes");
    invalid = stored;
    invalid["answers"]["checks"] = Json::array({"unit", false});
    expectDecodeError(invalid.dump(), "answers.checks");
    invalid = stored;
    invalid.erase("stopThreadId");
    expectDecodeError(invalid.dump(), "stopThreadId");
    invalid = stored;
    invalid.erase("stopPolicyOrigin");
    expectDecodeError(invalid.dump(), "stopPolicyOrigin");
    invalid = stored;
    invalid.erase("stopRequested");
    expectDecodeError(invalid.dump(), "stopRequested");
}
}

int main() {
    try {
        roundTripPhases();
        roundTripDeferredAndAnswerCommands();
        rejectsInvalidRows();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
