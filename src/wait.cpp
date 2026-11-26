#include "peer_storage.hpp"
#include "outbox_storage.hpp"
#include "journal_storage.hpp"
#include "database.hpp"
#include "storage.hpp"
#include "record_decode.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

#include <cstdint>
#include <limits>
#include <map>
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
    std::pair{WaitReason::budget, std::string_view{"budget"}},
    std::pair{WaitReason::model_budget, std::string_view{"model_budget"}},
    std::pair{WaitReason::deadline, std::string_view{"deadline"}},
    std::pair{WaitReason::model_failure, std::string_view{"model_failure"}},
    std::pair{WaitReason::bot_notes_unavailable, std::string_view{"bot_notes_unavailable"}},
    std::pair{WaitReason::t3_input, std::string_view{"t3_input"}},
    std::pair{WaitReason::auth, std::string_view{"auth"}},
    std::pair{WaitReason::settings, std::string_view{"settings"}},
    std::pair{WaitReason::unknown_delivery, std::string_view{"unknown_delivery"}},
    std::pair{WaitReason::policy_approval, std::string_view{"policy_approval"}},
};

constexpr std::array workTools{
    std::pair{WorkBudgetTool::delegate, std::string_view{"delegate"}},
    std::pair{WorkBudgetTool::follow_up, std::string_view{"follow_up"}},
    std::pair{WorkBudgetTool::send_bot_message, std::string_view{"send_bot_message"}},
};

constexpr std::string_view unsupportedEffort = "unsupported_reasoning_effort";
constexpr std::string_view projectQuestionReason = "project_question";

}

std::string_view waitReasonName(WaitReason reason) {
    return enumName(reasons, reason);
}

WaitReason waitReason(SimpleWaitReason reason) {
    switch (reason) {
    case SimpleWaitReason::model_budget: return WaitReason::model_budget;
    case SimpleWaitReason::bot_notes_unavailable: return WaitReason::bot_notes_unavailable;
    case SimpleWaitReason::t3_input: return WaitReason::t3_input;
    case SimpleWaitReason::auth: return WaitReason::auth;
    case SimpleWaitReason::settings: return WaitReason::settings;
    case SimpleWaitReason::unknown_delivery: return WaitReason::unknown_delivery;
    }
    throw std::logic_error("Invalid simple wait reason");
}

WaitReason Wait::reason() const {
    return std::visit(
        []<typename T>(const T& value) {
            if constexpr (std::is_same_v<T, SimpleWaitReason>) return waitReason(value);
            if constexpr (std::is_same_v<T, ModelQuestionWait> || std::is_same_v<T, ProjectQuestionWait>) return WaitReason::user_question;
            if constexpr (std::is_same_v<T, WorkBudgetWait>) return WaitReason::budget;
            if constexpr (std::is_same_v<T, DeadlineWait>) return WaitReason::deadline;
            if constexpr (std::is_same_v<T, ModelFailureWait>) return WaitReason::model_failure;
            if constexpr (std::is_same_v<T, PolicyApprovalWait>) return WaitReason::policy_approval;
        },
        details);
}

std::span<const std::string> Wait::choices() const {
    if (const auto* question = std::get_if<ModelQuestionWait>(&details)) return question->choices;
    if (const auto* question = std::get_if<ProjectQuestionWait>(&details)) return question->choices;
    return {};
}

const VerificationDeadline* Wait::verification() const {
    const auto* deadline = std::get_if<DeadlineWait>(&details);
    return deadline && deadline->verification ? &*deadline->verification : nullptr;
}

WorkBudgetTool workBudgetTool(std::string_view tool) {
    if (const auto known = enumValue(workTools, tool)) return *known;
    throw std::logic_error("Invalid follow-up work tool");
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
            } else if constexpr (std::is_same_v<T, ProjectQuestionWait>) {
                data["choices"] = value.choices;
                data["projectChoices"] = value.projects;
            } else if constexpr (std::is_same_v<T, WorkBudgetWait>) {
                data["attempted"] = enumName(workTools, value.attempted);
                data["decisionReason"] = value.decisionReason;
                const auto& usage = value.usage;
                Json measurements = Json::object();
                if (usage.inputTokens) measurements["input_tokens"] = *usage.inputTokens;
                if (usage.outputTokens) measurements["output_tokens"] = *usage.outputTokens;
                if (usage.totalTokens) measurements["total_tokens"] = *usage.totalTokens;
                if (usage.cachedTokens) measurements["input_tokens_details"] = {{"cached_tokens", *usage.cachedTokens}};
                if (usage.wallMs) measurements["wallMs"] = *usage.wallMs;
                data["usage"] = std::move(measurements);
            } else if constexpr (std::is_same_v<T, DeadlineWait>) {
                if (value.verification) {
                    data["deadline"] = value.verification->deadline;
                    data["runActionId"] = value.verification->actionId;
                }
            } else if constexpr (std::is_same_v<T, ModelFailureWait>) {
                if (value.code) data["failureCode"] = unsupportedEffort;
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

std::string_view storedWaitReason(const Wait& wait) {
    return std::holds_alternative<ProjectQuestionWait>(wait.details) ? projectQuestionReason : waitReasonName(wait.reason());
}

WaitUsage waitUsage(const Json& data) {
    const auto count = [](const Json& record, std::string_view key) -> std::optional<Id> {
        const auto found = record.find(key);
        if (found == record.end() || !found->is_number_integer() ||
            (found->is_number_unsigned() && found->get<std::uint64_t>() > std::numeric_limits<Id>::max()))
            return std::nullopt;
        return found->get<Id>();
    };
    const auto cached = data.find("input_tokens_details");
    return {count(data, "input_tokens"), count(data, "output_tokens"), count(data, "total_tokens"),
            cached == data.end() ? std::nullopt : count(*cached, "cached_tokens"), count(data, "wallMs")};
}

namespace {
WaitReason readWaitReason(const RecordDecoder& decoder, std::string_view name) {
    const auto reason = enumValue(reasons, name);
    if (!reason) decoder.fail("reason", "unknown wait reason");
    return *reason;
}

ProjectQuestionWait readProjectQuestion(const RecordDecoder& decoder, const Json& data) {
    auto choices = decoder.get<std::vector<std::string>>(data, "choices");
    const auto& projects = decoder.object(decoder.require(data, "projectChoices"), "projectChoices");
    const auto project = decoder.nested("projectChoices");
    std::map<std::string, std::string> ids;
    for (const auto& [name, id] : projects.items()) ids.emplace(name, project.value<std::string>(id, name));
    return {std::move(choices), std::move(ids)};
}

WaitDetails readWaitDetails(const RecordDecoder& decoder, const Json& data, WaitReason reason) {
    switch (reason) {
    case WaitReason::user_question:
        return ModelQuestionWait{decoder.get<std::vector<std::string>>(data, "choices"), decoder.get<std::string>(data, "decisionReason"),
                                 decoder.get<std::string>(data, "questionId")};
    case WaitReason::budget: {
        const auto attempted = enumValue(workTools, decoder.get<std::string>(data, "attempted"));
        if (!attempted) decoder.fail("attempted", "unknown work tool");
        const auto& usage = decoder.object(decoder.require(data, "usage"), "usage");
        const auto item = decoder.nested("usage");
        std::optional<Id> cached;
        if (usage.contains("input_tokens_details")) {
            const auto details = item.nested("input_tokens_details");
            cached = details.get<Id>(usage.at("input_tokens_details"), "cached_tokens");
        }
        return WorkBudgetWait{*attempted,
                              decoder.get<std::string>(data, "decisionReason"),
                              {item.optional<Id>(usage, "input_tokens"), item.optional<Id>(usage, "output_tokens"),
                               item.optional<Id>(usage, "total_tokens"), cached, item.optional<Id>(usage, "wallMs")}};
    }
    case WaitReason::deadline:
        if (data.contains("deadline") || data.contains("runActionId"))
            return DeadlineWait{
                VerificationDeadline{decoder.get<Seconds>(data, "deadline"), decoder.get<std::string>(data, "runActionId")}};
        return DeadlineWait{};
    case WaitReason::model_failure:
        if (const auto code = decoder.optional<std::string>(data, "failureCode")) {
            if (*code != unsupportedEffort) decoder.fail("failureCode", "unknown model failure code");
            return ModelFailureWait{WaitFailureCode::unsupported_reasoning_effort};
        }
        return ModelFailureWait{};
    case WaitReason::policy_approval: return PolicyApprovalWait{decoder.get<Id>(data, "approvalId")};
    case WaitReason::model_budget: return SimpleWaitReason::model_budget;
    case WaitReason::bot_notes_unavailable: return SimpleWaitReason::bot_notes_unavailable;
    case WaitReason::t3_input: return SimpleWaitReason::t3_input;
    case WaitReason::auth: return SimpleWaitReason::auth;
    case WaitReason::settings: return SimpleWaitReason::settings;
    case WaitReason::unknown_delivery: return SimpleWaitReason::unknown_delivery;
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
        auto details = reason == projectQuestionReason ? WaitDetails{readProjectQuestion(fields, payload)}
                                                       : readWaitDetails(fields, payload, readWaitReason(fields, reason));
        auto wait = readWaitPayload(fields, payload, std::move(details));
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
        auto details = reason == WaitReason::user_question && payload.contains("projectChoices")
                           ? WaitDetails{readProjectQuestion(fields, payload)}
                           : readWaitDetails(fields, payload, reason);
        auto wait = readWaitPayload(fields, payload, std::move(details));
        wait.id = fields.get<Id>(payload, "id");
        wait.messageId = fields.optional<Id>(payload, "messageId");
        wait.allowance = fields.optional<Id>(payload, "allowance");
        return wait;
    });
}

Wait Store::waitingEvent(const Json& event) {
    return decodeWaitingEntry(event.at("id"), event.at("data"));
}

Id WaitStorage::waitAnchor(Id task, Id wait) const {
    auto row = database.query("SELECT id FROM journal WHERE task_id=? AND kind='waiting' AND conversation_bot_id IS NULL"
                              " AND wait_id=? ORDER BY id DESC LIMIT 1",
                              {task, wait});
    return row.next() ? row.number(0) : 0;
}

Id WaitStorage::waitAllowance(Id task, WaitReason reason) const {
    if (reason == WaitReason::model_budget) return remaining.taskModelBudget(task).initialLimit;
    if (reason == WaitReason::budget) return initialWorkAllowance;
    return 0;
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
    if (const auto allowance = waitAllowance(task, wait.reason())) wait.allowance = allowance;
    return wait;
}

Id WaitStorage::wait(TaskData& task, WaitDetails details, std::string text, std::optional<WaitRun> run) {
    return wait(task, {.details = std::move(details), .text = std::move(text), .run = run ? std::move(run) : taskWaitRun(task)});
}

Id WaitStorage::wait(TaskData& task, Wait waiting) {
    clearWait(task, "superseded");
    waiting.eventId = task.eventId;
    if (!waiting.run && waiting.reason() == WaitReason::unknown_delivery) {
        const auto action = outboxStorage.outbox(task);
        if (action && action->command.candidate && !action->command.candidate->link.empty())
            waiting.run = WaitRun{*action->command.candidate};
    }
    waiting.actionId = task.actionId;
    database.exec("INSERT INTO waits(task_id,reason,data) VALUES(?,?,?)", {task.id, storedWaitReason(waiting), encodeWaitData(waiting)});
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
    journalStorage.journal(task.id, "wait_closed", {{"waitId", id}, {"resolution", resolution}});
}

void WaitStorage::unlockQuestion(TaskData& task, std::string_view reason) {
    if (!task.question) return;
    journalStorage.journal(task.id, "session_question_closed", {{"requestId", task.question->target.requestId}, {"resolution", reason}});
    task.question.reset();
}

}
