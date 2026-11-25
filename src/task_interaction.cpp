#include "task_interaction_storage.hpp"

#include "record_decode.hpp"
#include "storage.hpp"
#include "task_storage.hpp"

namespace flockv0 {
namespace {
Json checkpointJson(const DecisionCheckpoint& checkpoint) {
    const auto& decision = checkpoint.decision;
    Json data{{"generation", checkpoint.generation},
              {"eventId", checkpoint.eventId},
              {"revisionId", checkpoint.revisionId ? Json(*checkpoint.revisionId) : Json(nullptr)},
              {"tool", decision.tool},
              {"args", decision.args},
              {"usage", decision.usage},
              {"attemptId", decision.attemptId}};
    if (!decision.reason.empty()) data["reason"] = decision.reason;
    if (!decision.rejection.empty()) data["rejection"] = decision.rejection;
    return data;
}

DecisionCheckpoint decodeCheckpoint(const Json& data, const RecordDecoder& row) {
    std::set<std::string_view> keys;
    const auto item = row.tracking(keys);
    DecisionCheckpoint checkpoint{
        item.get<Id>(data, "generation"),
        item.get<Id>(data, "eventId"),
        item.nullable<Id>(data, "revisionId"),
        {{item.get<std::string>(data, "tool"), item.require(data, "args"), item.optional<std::string>(data, "reason").value_or("")},
         item.require(data, "usage"),
         item.get<Id>(data, "attemptId"),
         item.optional<std::string>(data, "rejection").value_or("")}};
    item.rejectUnknown(data);
    return checkpoint;
}

QuestionOption decodeQuestionOption(const Json& data, const RecordDecoder& row) {
    std::set<std::string_view> keys;
    const auto item = row.tracking(keys);
    QuestionOption option{item.get<std::string>(data, "label"), item.get<std::string>(data, "value"),
                          item.get<std::string>(data, "description")};
    item.rejectUnknown(data);
    return option;
}

QuestionField decodeQuestionField(const Json& data, const RecordDecoder& row) {
    std::set<std::string_view> keys;
    const auto item = row.tracking(keys);
    QuestionField field{.id = item.get<std::string>(data, "id"),
                        .prompt = item.get<std::string>(data, "question"),
                        .header = item.get<std::string>(data, "header"),
                        .required = item.get<bool>(data, "required"),
                        .multiple = item.get<bool>(data, "multiple"),
                        .freeform = item.get<bool>(data, "freeform"),
                        .initialAnswer = item.optional<std::string>(data, "initialAnswer")};
    for (const auto& option : item.array(item.require(data, "options"), "options"))
        field.options.push_back(decodeQuestionOption(option, row.nested("options." + std::to_string(field.options.size()))));
    item.rejectUnknown(data);
    return field;
}

QuestionTarget decodeQuestionTarget(const Json& data, const RecordDecoder& row) {
    std::set<std::string_view> keys, runKeys;
    const auto item = row.tracking(keys);
    const auto& run = item.require(data, "run");
    const auto runRow = row.nested("run").tracking(runKeys);
    QuestionTarget target{item.nullable<Id>(data, "waitId"), item.get<Id>(data, "eventId"), decodeWaitRun(runRow, run),
                          item.get<std::string>(data, "requestId")};
    if (!run.is_null()) runRow.rejectUnknown(run);
    item.rejectUnknown(data);
    return target;
}

TaskQuestion decodeTaskQuestion(const Json& data, const RecordDecoder& row) {
    std::set<std::string_view> keys;
    const auto item = row.tracking(keys);
    TaskQuestion question{
        {}, decodeQuestionTarget(item.require(data, "target"), row.nested("target")), item.optional<Id>(data, "suspendedWaitId")};
    for (const auto& field : item.array(item.require(data, "fields"), "fields"))
        question.fields.push_back(decodeQuestionField(field, row.nested("fields." + std::to_string(question.fields.size()))));
    if (const auto found = item.find(data, "suspendedDecision"); found != data.end())
        question.suspendedDecision = decodeCheckpoint(*found, row.nested("suspendedDecision"));
    if (question.target.requestId != item.get<std::string>(data, "requestId")) item.fail("requestId", "does not match the question target");
    item.rejectUnknown(data);
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
    if (question.suspendedDecision) data["suspendedDecision"] = checkpointJson(*question.suspendedDecision);
    return data;
}
}

void decodeTaskInteraction(TaskData& task, const Json& data, const RecordDecoder& row) {
    const auto& question = row.require(data, "question");
    if (!question.is_null()) task.question = decodeTaskQuestion(question, row.nested("question"));
    if (const auto found = row.find(data, "pendingDecision"); found != data.end())
        task.pendingDecision = decodeCheckpoint(*found, row.nested("pendingDecision"));
    const auto waitId = row.optional<Id>(data, "blockerWaitId");
    if (auto reason = row.optional<std::string>(data, "blockerReason"))
        task.blocker = RecordedTaskBlocker{std::move(*reason), waitId};
    else if (waitId)
        row.fail("blockerWaitId", "requires a blocker reason");
}

void encodeTaskInteraction(Json& data, const TaskData& task) {
    data["question"] = task.question ? taskQuestionJson(*task.question) : Json(nullptr);
    if (task.pendingDecision) data["pendingDecision"] = checkpointJson(*task.pendingDecision);
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

void Store::Impl::saveDecisionCheckpoint(TaskData& task, DecisionCheckpoint checkpoint) {
    task.pendingDecision = std::move(checkpoint);
    exec("UPDATE tasks SET data=? WHERE id=?", {encodeTask(task), task.id});
}
}
