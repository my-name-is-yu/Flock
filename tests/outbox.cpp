#include "../src/outbox_storage.hpp"
#include "support.hpp"
#include "../src/database.hpp"

#include "flock/record_decode_error.hpp"
#include "flock/command.hpp"
#include "flock/json.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace std::literals;
using namespace flockv0;

namespace {
constexpr auto id = "outbox-test-action"sv;
constexpr auto authGeneration = "auth-generation"sv;

using flockv0::test::check;

Json payload() {
    const Json source = {{"taskId", 11},
                         {"messageId", 12},
                         {"revisionId", 13},
                         {"run", {{"threadId", "source-thread"}, {"runId", "source-run"}, {"itemId", "source-item"}}}};
    auto instructionSource = source;
    instructionSource["text"] = "Earlier result";
    return {{"target", {{"threadId", "thread"}, {"runId", "run"}, {"link", "link"}, {"gatewayEnvironmentId", "gateway"}}},
            {"gatewayEnvironmentId", "gateway"},
            {"questionId", ""},
            {"answers", Json::object()},
            {"instructions", Json::array({{{"messageId", 14},
                                           {"step", 2},
                                           {"text", "Earlier direction"},
                                           {"revisionId", nullptr},
                                           {"conflicting", false},
                                           {"source", instructionSource}}})},
            {"contextIds", {{"answerEventId", 15}, {"sourceMessageId", 17}}},
            {"sourceResultRefs", Json::array({source})},
            {"destinationAction", true},
            {"expectedCommit", "abc123"},
            {"receipt", {{"threadId", "receipt-thread"}, {"runId", "receipt-run"}, {"link", "receipt-link"}}},
            {"stopRunId", "stop-run"},
            {"stopThreadId", "stop-thread"},
            {"stopPolicy",
             {{"tool", "cancel_task"}, {"args", {{"taskId", 11}, {"destination", "left"}}}, {"generation", 22}, {"revisionId", 13}}},
            {"stopPolicyOrigin", {{"scope", "task/11"}, {"requester", "user"}, {"anchor", 24}, {"turn", 0}, {"background", false}}}};
}

Json storedAction() {
    return Json::array({"follow_up", "environment", "project", 13, 18, "stop-action", "Continue the work", 0, 0, "Session notes", 21,
                        "role-hash", "Standing rules", 14, payload()});
}

Outbox decode(Json columns, std::string_view phase = "ready", std::string_view auth = authGeneration) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(":memory:", &raw);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    check(opened == SQLITE_OK, "Could not open the outbox row fixture");
    columns.insert(columns.begin(), {id, phase, auth});
    Statement row(db.get(), "SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?", columns);
    check(row.next(), "Outbox row fixture returned no rows");
    return decodeOutboxRow(row);
}

void roundTripPhases() {
    using enum OutboxPhase;
    constexpr std::array phases{
        std::pair{ready, "ready"sv},   std::pair{started, "started"sv},       std::pair{accepted, "accepted"sv},
        std::pair{closed, "closed"sv}, std::pair{superseded, "superseded"sv},
    };
    const auto stored = storedAction();
    for (const auto& [phase, name] : phases) {
        const auto row = decode(stored, name);
        check(row.phase == phase && outboxPhaseName(row.phase) == name, "Outbox phase did not round-trip");
        check(row.command.actionId == id && row.authGeneration == authGeneration, "Outbox lost its ID or auth generation");
        check(row.command.tool == CommandTool::follow_up && row.command.target.runId == "run", "Outbox lost its follow-up target");
        check(row.command.environmentId == "environment" && row.command.projectId == "project" && row.revisionId == 13 &&
                  row.sessionParticipationId == 18 && row.command.instruction == "Continue the work",
              "Outbox lost its normalized action fields");
        check(row.stop && row.stop->run && row.stop->run->runId == "stop-run" && row.stop->approval &&
                  row.stop->approval->scope == "task/11" && row.stop->operation == "stop-action",
              "Outbox lost its stop request");
        const auto& [role, memory] = row.sessionContext;
        check(memory && memory->version == 0 && memory->section == "Session notes" && memory->excerptBytes == 21,
              "Outbox lost its version-zero memory snapshot");
        check(role && role->version == "role-hash" && role->section == "Standing rules" && role->excerptBytes == 14,
              "Outbox lost its Role snapshot");
        const auto encoded = outboxColumnValues(row);
        check(encoded == stored, "Outbox columns and payload changed on round-trip. Diff " + Json::diff(stored, encoded).dump());
        const auto wire = outboxActionJson(row);
        check(wire.at("tool") == "follow_up" && wire.at("environmentId") == "environment" && wire.at("projectId") == "project" &&
                  wire.at("revisionId") == 13 && wire.at("sessionParticipationId") == 18 && wire.at("stopRequested") == true &&
                  wire.at("stopOperation") == "stop-action" && wire.at("instruction") == "Continue the work" &&
                  !wire.contains("sessionContext") && !wire.contains("sessionRole") && !wire.contains("sessionMemory"),
              "Stored columns changed the outbox action projection");
    }
}

void roundTripDeferredAndAnswerCommands() {
    Outbox deferred;
    deferred.command.tool = CommandTool::delegate;
    deferred.command.environmentId = "environment";
    deferred.command.projectId = "project";
    deferred.instructionBase = "Start the work";
    auto stored = outboxColumnValues(deferred);
    auto row = decode(stored, "ready", "");
    const auto wire = outboxActionJson(row);
    check(wire.at("target").is_null() && wire.at("revisionId").is_null() && !wire.contains("instruction") &&
              wire.at("instructionBase") == "Start the work",
          "A deferred launch projected a concrete target, revision or instruction");
    check(row.instructionBase == deferred.instructionBase && !row.revisionId && row.command.target.threadId.empty() &&
              !row.command.receipt && !row.stop && !row.sessionContext.role && !row.sessionContext.memory &&
              outboxColumnValues(row) == stored,
          "A deferred launch did not round-trip");

    deferred.command.tool = CommandTool::start_in_session;
    deferred.command.target.threadId = "selected";
    stored = outboxColumnValues(deferred);
    row = decode(stored, "ready", "");
    check(row.command.tool == CommandTool::start_in_session && row.command.target.threadId == "selected" &&
              outboxColumnValues(row) == stored,
          "A deferred Session start did not round-trip");

    Outbox reply;
    reply.command.tool = CommandTool::answer_session_question;
    reply.command.target = {"thread", "run", "link", {}};
    reply.command.questionId = "question";
    reply.command.answers = {{"branch", "main"}, {"checks", Json::array({"unit", "integration"})}};
    stored = outboxColumnValues(reply);
    row = decode(stored, "closed");
    check(row.command.tool == CommandTool::answer_session_question && row.command.questionId == "question" &&
              row.command.answers == reply.command.answers && !row.instructionBase && row.command.instruction.empty() &&
              outboxColumnValues(row) == stored,
          "A Session answer did not round-trip");
}

void expectDecodeError(const Json& stored, std::string_view key, std::string_view phase = "ready") {
    try {
        decode(stored, phase);
    } catch (const RecordDecodeError& error) {
        const auto expected = "outbox row " + std::string(id) + ", key " + std::string(key) + ": ";
        check(std::string_view(error.what()).starts_with(expected), error.what());
        return;
    }
    throw std::runtime_error("Outbox decode accepted invalid " + std::string(key));
}

void rejectsInvalidRows() {
    const auto stored = storedAction();
    auto invalid = stored;
    invalid.back() = "{";
    expectDecodeError(invalid, "data");
    expectDecodeError(stored, "phase", "dispatched");
    invalid = stored;
    invalid.at(0) = "send";
    expectDecodeError(invalid, "tool");
    invalid.at(0) = "message_session";
    expectDecodeError(invalid, "tool");
    invalid = stored;
    invalid.at(7) = 1;
    expectDecodeError(invalid, "instructionBase");
    invalid = stored;
    invalid.back()["instructions"][0]["step"] = "2";
    expectDecodeError(invalid, "instructions.0.step");
    invalid = stored;
    invalid.at(10) = -1;
    expectDecodeError(invalid, "sessionMemory.excerptBytes");
    invalid = stored;
    invalid.at(13) = -1;
    expectDecodeError(invalid, "sessionRole.excerptBytes");
    invalid = stored;
    invalid.back()["answers"]["checks"] = Json::array({"unit", false});
    expectDecodeError(invalid, "answers.checks");
    invalid = stored;
    invalid.back().erase("stopThreadId");
    expectDecodeError(invalid, "stopThreadId");
    invalid = stored;
    invalid.back().erase("stopPolicyOrigin");
    expectDecodeError(invalid, "stopPolicyOrigin");
    invalid = stored;
    invalid.at(5) = nullptr;
    expectDecodeError(invalid, "stopOperation");
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
