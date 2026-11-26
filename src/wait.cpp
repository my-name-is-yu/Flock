#include "flock/task_history.hpp"
#include "task_operations.hpp"
#include "task_records.hpp"
#include "wait_storage.hpp"
#include "run_ref_storage.hpp"
#include "peer_storage.hpp"
#include "journal_storage.hpp"
#include "database.hpp"
#include "record_decode.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr std::array reasons{
    std::pair{WaitReason::user_question, std::string_view{"user_question"}},
    std::pair{WaitReason::model_failure, std::string_view{"model_failure"}},
    std::pair{WaitReason::bot_notes_unavailable, std::string_view{"bot_notes_unavailable"}},
    std::pair{WaitReason::t3_input, std::string_view{"t3_input"}},
    std::pair{WaitReason::auth, std::string_view{"auth"}},
    std::pair{WaitReason::settings, std::string_view{"settings"}},
    std::pair{WaitReason::policy_approval, std::string_view{"policy_approval"}},
};

constexpr std::array modelFailures{
    std::pair{WaitFailureCode::unsupported_reasoning_effort, std::string_view{"unsupported_reasoning_effort"}},
    std::pair{WaitFailureCode::authentication_required, std::string_view{"authentication_required"}},
};

}

std::string_view waitReasonName(WaitReason reason) {
    return enumName(reasons, reason);
}

WaitReason waitReason(SimpleWaitReason reason) {
    switch (reason) {
    case SimpleWaitReason::bot_notes_unavailable: return WaitReason::bot_notes_unavailable;
    case SimpleWaitReason::t3_input: return WaitReason::t3_input;
    case SimpleWaitReason::auth: return WaitReason::auth;
    case SimpleWaitReason::settings: return WaitReason::settings;
    }
    throw std::logic_error("Invalid simple wait reason");
}

WaitReason Wait::reason() const {
    return std::visit(
        []<typename T>(const T& value) {
            if constexpr (std::is_same_v<T, SimpleWaitReason>) return waitReason(value);
            if constexpr (std::is_same_v<T, ModelQuestionWait>) return WaitReason::user_question;
            if constexpr (std::is_same_v<T, ModelFailureWait>) return WaitReason::model_failure;
            if constexpr (std::is_same_v<T, PolicyApprovalWait>) return WaitReason::policy_approval;
        },
        details);
}

std::span<const std::string> Wait::choices() const {
    if (const auto* question = std::get_if<ModelQuestionWait>(&details)) return question->choices;
    return {};
}

Json waitRunJson(const std::optional<WaitRun>& run) {
    if (!run) return nullptr;
    auto result = runJson(run->run);
    if (run->environmentId) result["environmentId"] = *run->environmentId;
    if (run->assignment) encodeRunAssignment(result, *run->assignment);
    return result;
}

std::optional<WaitRun> taskWaitRun(const TaskData& task) {
    if (!task.activeRun) return std::nullopt;
    const auto& run = *task.activeRun;
    return WaitRun{run.ref, run.environmentId, run.assignment};
}

Json encodeWaitData(const Wait& wait) {
    Json data{{"text", wait.text}, {"eventId", wait.eventId}, {"run", waitRunJson(wait.run)}};
    if (wait.actionId) data["actionId"] = *wait.actionId;
    std::visit(
        [&]<typename T>(const T& value) {
            if constexpr (std::is_same_v<T, ModelQuestionWait>) {
                data["choices"] = value.choices;
                data["decisionReason"] = value.decisionReason;
                data["questionId"] = value.questionId;
            } else if constexpr (std::is_same_v<T, ModelFailureWait>) {
                if (value.code) data["failureCode"] = enumName(modelFailures, *value.code);
            } else if constexpr (std::is_same_v<T, PolicyApprovalWait>) {
                data["approvalId"] = value.approvalId;
            }
        },
        wait.details);
    return data;
}

Json to_json(const Wait& wait) {
    auto data = encodeWaitData(wait);
    data["reason"] = waitReasonName(wait.reason());
    return data;
}

namespace {
WaitReason readWaitReason(const RecordDecoder& decoder, std::string_view name) {
    const auto reason = enumValue(reasons, name);
    if (!reason) decoder.fail("reason", "unknown wait reason");
    return *reason;
}

WaitDetails readWaitDetails(const RecordDecoder& decoder, const Json& data, WaitReason reason) {
    switch (reason) {
    case WaitReason::user_question:
        return ModelQuestionWait{decoder.get<std::vector<std::string>>(data, "choices"), decoder.get<std::string>(data, "decisionReason"),
                                 decoder.get<std::string>(data, "questionId")};
    case WaitReason::model_failure:
        if (const auto code = decoder.optional<std::string>(data, "failureCode")) {
            const auto value = enumValue(modelFailures, *code);
            if (!value) decoder.fail("failureCode", "unknown model failure code");
            return ModelFailureWait{*value};
        }
        return ModelFailureWait{};
    case WaitReason::policy_approval: return PolicyApprovalWait{decoder.get<Id>(data, "approvalId")};
    case WaitReason::bot_notes_unavailable: return SimpleWaitReason::bot_notes_unavailable;
    case WaitReason::t3_input: return SimpleWaitReason::t3_input;
    case WaitReason::auth: return SimpleWaitReason::auth;
    case WaitReason::settings: return SimpleWaitReason::settings;
    }
    throw std::logic_error("Invalid wait reason");
}

}

std::optional<WaitRun> decodeWaitRun(const Json& data, const RecordDecoder& decoder) {
    if (data.is_null()) return std::nullopt;
    WaitRun run{decodeRunRef(data, decoder), decoder.optional<std::string>(data, "environmentId"),
                decodeOptionalRunAssignment(data, decoder)};
    if (run.assignment && !run.environmentId) decoder.fail("environmentId", "missing required key");
    return run;
}

namespace {
Wait readWaitPayload(const RecordDecoder& decoder, const Json& data, WaitDetails details) {
    Wait wait{std::move(details), decoder.get<std::string>(data, "text")};
    wait.eventId = decoder.get<Id>(data, "eventId");
    wait.actionId = decoder.optional<std::string>(data, "actionId");
    const auto& run = decoder.require(data, "run");
    if (!run.is_null()) wait.run = decoder.nested("run").decodeClosed(run, decodeWaitRun);
    return wait;
}
}

Wait decodeWait(Id id, std::string_view reason, std::string_view data) {
    const auto row = std::to_string(id);
    const RecordDecoder decoder("waits", row);
    return decoder.decodeClosed(decoder.parse(data), [&](const Json& payload, const RecordDecoder& fields) {
        auto wait = readWaitPayload(fields, payload, readWaitDetails(fields, payload, readWaitReason(fields, reason)));
        wait.id = id;
        return wait;
    });
}

Wait decodeWaitRow(const Statement& row, int col) {
    return decodeWait(row.number(col), row.string(col + 1), row.string(col + 2));
}

Wait decodeWaitingEntry(Id entry, const Json& data) {
    const auto row = std::to_string(entry);
    const RecordDecoder decoder("journal", row);
    return decoder.decodeClosed(data, [](const Json& payload, const RecordDecoder& fields) {
        const auto reason = readWaitReason(fields, fields.get<std::string>(payload, "reason"));
        auto wait = readWaitPayload(fields, payload, readWaitDetails(fields, payload, reason));
        wait.id = fields.get<Id>(payload, "id");
        wait.messageId = fields.optional<Id>(payload, "messageId");
        return wait;
    });
}

Wait TaskHistory::waitingEvent(const Json& event) {
    return decodeWaitingEntry(event.at("id"), event.at("data"));
}

Id WaitStorage::waitAnchor(Id task, Id wait) const {
    auto row = database.query("SELECT id FROM journal WHERE task_id=? AND kind='waiting' AND conversation_bot_id IS NULL"
                              " AND wait_id=? ORDER BY id DESC LIMIT 1",
                              {task, wait});
    return row.next() ? row.number(0) : 0;
}

bool WaitStorage::waitIsActionable(const TaskData& task) const {
    return !peers.replanReady(task);
}

std::optional<Wait> WaitStorage::currentWait(Id task) const {
    auto row = database.query("SELECT id,reason,data FROM waits WHERE task_id=? AND resolution IS NULL", {task});
    if (!row.next()) return std::nullopt;
    return decodeWaitRow(row);
}

std::optional<Wait> WaitStorage::projectedWait(Id task, const Statement& row, int col) const {
    const auto id = row.number(col);
    if (!id) return std::nullopt;
    auto wait = decodeWaitRow(row, col);
    if (const auto anchor = waitAnchor(task, wait.id)) wait.messageId = anchor;
    return wait;
}

Id WaitStorage::wait(TaskData& task, WaitDetails details, std::string text, std::optional<WaitRun> run) {
    return wait(task, {.details = std::move(details), .text = std::move(text), .run = run ? std::move(run) : taskWaitRun(task)});
}

Id WaitStorage::wait(TaskData& task, Wait waiting) {
    clearWait(task, "superseded");
    waiting.eventId = task.eventId;
    waiting.actionId = task.actionId;
    database.exec("INSERT INTO waits(task_id,reason,data) VALUES(?,?,?)",
                  {task.id, waitReasonName(waiting.reason()), encodeWaitData(waiting)});
    waiting.id = database.last();
    journalStorage.journal(task.id, waiting);
    task.state = TaskState::waiting;
    return waiting.id;
}

void WaitStorage::clearWait(TaskData& task, std::string_view resolution) {
    auto row = database.query("UPDATE waits SET resolution=? WHERE task_id=? AND resolution IS NULL RETURNING id", {resolution, task.id});
    if (!row.next()) return;
    const auto id = row.number(0);
    row.next();
    if (task.blocker && task.blocker->waitId == id) task.blocker.reset();
    database.writeJournal(task.id, "wait_closed", {{"waitId", id}, {"resolution", resolution}});
}

void WaitStorage::unlockQuestion(TaskData& task, std::string_view reason) {
    if (!task.question) return;
    database.writeJournal(task.id, "session_question_closed", {{"requestId", task.question->target.requestId}, {"resolution", reason}});
    task.question.reset();
}

}
