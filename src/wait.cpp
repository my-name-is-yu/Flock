#include "storage.hpp"
#include "record_decode.hpp"

#include <limits>

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

Json to_json(const Wait& wait) {
    Json data{{"reason", waitReasonName(wait.reason())}, {"text", wait.text}, {"eventId", wait.eventId}, {"run", waitRunJson(wait.run)}};
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
WaitDetails readWaitDetails(const RecordDecoder& decoder, const Json& data, WaitReason reason) {
    switch (reason) {
    case WaitReason::user_question: {
        auto choices = decoder.get<std::vector<std::string>>(data, "choices");
        if (!data.contains("projectChoices"))
            return ModelQuestionWait{std::move(choices), decoder.get<std::string>(data, "decisionReason"),
                                     decoder.get<std::string>(data, "questionId")};
        const auto& projects = decoder.object(data.at("projectChoices"), "projectChoices");
        std::map<std::string, std::string> ids;
        for (const auto& [name, id] : projects.items()) ids.emplace(name, decoder.nested("projectChoices").value<std::string>(id, name));
        return ProjectQuestionWait{std::move(choices), std::move(ids)};
    }
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

std::optional<WaitRun> decodeRun(const RecordDecoder& decoder, const Json& data) {
    if (data.is_null()) return std::nullopt;
    WaitRun run{decodeRunRef(data, decoder), decoder.optional<std::string>(data, "environmentId"),
                decodeOptionalRunAssignment(data, decoder)};
    if (run.assignment && !run.environmentId) decoder.fail("environmentId", "missing required key");
    return run;
}

Wait readWaitPayload(const RecordDecoder& decoder, const Json& data) {
    const auto reason = enumValue(reasons, decoder.get<std::string>(data, "reason"));
    if (!reason) decoder.fail("reason", "unknown wait reason");
    Wait wait{readWaitDetails(decoder, data, *reason), decoder.get<std::string>(data, "text")};
    wait.eventId = decoder.get<Id>(data, "eventId");
    wait.actionId = decoder.optional<std::string>(data, "actionId");
    wait.run = decodeRun(decoder.nested("run"), decoder.require(data, "run"));
    return wait;
}
}

Wait from_json(Id id, std::string_view data) {
    const auto row = std::to_string(id);
    const RecordDecoder decoder("waits", row);
    auto wait = readWaitPayload(decoder, decoder.parse(data));
    wait.id = id;
    return wait;
}

Wait decodeWaitingEntry(Id entry, const Json& data) {
    const auto row = std::to_string(entry);
    const RecordDecoder decoder("journal", row);
    auto wait = readWaitPayload(decoder, data);
    wait.id = decoder.get<Id>(data, "id");
    wait.messageId = decoder.optional<Id>(data, "messageId");
    wait.allowance = decoder.optional<Id>(data, "allowance");
    return wait;
}

Wait Store::waitingEvent(const Json& event) {
    return decodeWaitingEntry(event.at("id"), event.at("data"));
}
}
