#include "wait_storage.hpp"
#include "flock/secrets.hpp"
#include "database.hpp"
#include "task_records.hpp"
#include "task_interaction_storage.hpp"
#include "record_decode.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/wait.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
QuestionOption decodeQuestionOption(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "label"), row.get<std::string>(data, "value"), row.get<std::string>(data, "description")};
}

QuestionField decodeQuestionField(const Json& data, const RecordDecoder& row) {
    QuestionField field{.id = row.get<std::string>(data, "id"),
                        .prompt = row.get<std::string>(data, "question"),
                        .header = row.get<std::string>(data, "header"),
                        .required = row.get<bool>(data, "required"),
                        .multiple = row.get<bool>(data, "multiple"),
                        .freeform = row.get<bool>(data, "freeform"),
                        .initialAnswer = row.optional<std::string>(data, "initialAnswer")};
    for (const auto& option : row.array(row.require(data, "options"), "options"))
        field.options.push_back(row.nested("options." + std::to_string(field.options.size())).decodeClosed(option, decodeQuestionOption));
    return field;
}

QuestionTarget decodeQuestionTarget(const Json& data, const RecordDecoder& row) {
    const auto& run = row.require(data, "run");
    QuestionTarget target{row.nullable<Id>(data, "waitId"), row.get<Id>(data, "eventId"), std::nullopt,
                          row.get<std::string>(data, "requestId")};
    if (!run.is_null()) target.run = row.nested("run").decodeClosed(run, decodeWaitRun);
    return target;
}

TaskQuestion decodeTaskQuestion(const Json& data, const RecordDecoder& row) {
    TaskQuestion question{{},
                          row.nested("target").decodeClosed(row.require(data, "target"), decodeQuestionTarget),
                          row.optional<Id>(data, "suspendedWaitId")};
    for (const auto& field : row.array(row.require(data, "fields"), "fields"))
        question.fields.push_back(row.nested("fields." + std::to_string(question.fields.size())).decodeClosed(field, decodeQuestionField));
    if (question.target.requestId != row.get<std::string>(data, "requestId")) row.fail("requestId", "does not match the question target");
    return question;
}

Json questionPayload(std::string_view requestId, std::span<const QuestionField> fields) {
    auto projected = Json::array();
    for (const auto& field : fields) {
        auto options = Json::array();
        for (const auto& option : field.options)
            options.push_back({{"label", option.label}, {"value", option.value}, {"description", option.description}});
        Json data{{"id", field.id},
                  {"question", field.prompt},
                  {"header", field.header},
                  {"required", field.required},
                  {"multiple", field.multiple},
                  {"freeform", field.freeform},
                  {"options", std::move(options)}};
        if (field.initialAnswer) data["initialAnswer"] = *field.initialAnswer;
        projected.push_back(std::move(data));
    }
    return {{"requestId", requestId}, {"fields", std::move(projected)}};
}

Json taskQuestionJson(const TaskQuestion& question) {
    auto data = questionPayload(question.target.requestId, question.fields);
    data["target"] = questionTargetJson(question.target);
    if (question.suspendedWaitId) data["suspendedWaitId"] = *question.suspendedWaitId;
    return data;
}
}

void decodeTaskInteraction(TaskData& task, const Json& data, const RecordDecoder& row) {
    const auto& question = row.require(data, "question");
    if (!question.is_null()) task.question = row.nested("question").decodeClosed(question, decodeTaskQuestion);
    const auto waitId = row.optional<Id>(data, "blockerWaitId");
    if (auto reason = row.optional<std::string>(data, "blockerReason"))
        task.blocker = RecordedTaskBlocker{std::move(*reason), waitId};
    else if (waitId)
        row.fail("blockerWaitId", "requires a blocker reason");
}

void encodeTaskInteraction(Json& data, const TaskData& task) {
    data["question"] = task.question ? taskQuestionJson(*task.question) : Json(nullptr);
    if (task.blocker) {
        data["blockerReason"] = task.blocker->reason;
        if (task.blocker->waitId) data["blockerWaitId"] = *task.blocker->waitId;
    }
}

Json questionJson(const Question& question) {
    return questionPayload(question.requestId, question.fields);
}

Json questionTargetJson(const QuestionTarget& target) {
    return {{"waitId", target.waitId ? Json(*target.waitId) : Json(nullptr)},
            {"eventId", target.eventId},
            {"run", waitRunJson(target.run)},
            {"requestId", target.requestId}};
}

QuestionTarget questionTarget(const TaskData& task, Id waitId) {
    return {waitId ? std::optional{waitId} : std::nullopt, task.eventId, taskWaitRun(task),
            task.question ? task.question->target.requestId : std::string{}};
}

QuestionTarget waitQuestionTarget(const Wait& wait) {
    const auto* question = std::get_if<ModelQuestionWait>(&wait.details);
    return {wait.id, wait.eventId, wait.run, question ? question->questionId : std::string{}};
}

void redactQuestion(Question& question, const SecretRedactor& scrub) {
    scrub.requirePublic(question.requestId);
    for (auto& field : question.fields) {
        for (auto* value : {&field.id, &field.prompt, &field.header}) *value = scrub.text(*value);
        if (field.initialAnswer) *field.initialAnswer = scrub.text(*field.initialAnswer);
        for (auto& option : field.options)
            for (auto* value : {&option.label, &option.value, &option.description}) *value = scrub.text(*value);
    }
}

}
