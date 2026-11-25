#include "task_context_storage.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"

#include <set>

namespace flockv0 {
namespace {
using namespace std::literals;

constexpr std::array timedKinds{
    std::pair{TimedTriggerKind::schedule, "schedule"sv},
    std::pair{TimedTriggerKind::reminder, "reminder"sv},
};

constexpr std::array peerOutcomes{
    std::pair{TaskPeerOutcome::completed, "completed"sv},
    std::pair{TaskPeerOutcome::failed, "failed"sv},
};

TaskRestart decodeRestart(const Json& data, const RecordDecoder& record) {
    std::set<std::string_view> keys;
    const auto row = record.tracking(keys);
    TaskRestart restart{row.get<Id>(data, "taskId"), {}};
    const auto& delivered = row.array(row.require(data, "deliveredInstructions"), "deliveredInstructions");
    for (const auto& instruction : delivered) {
        std::set<std::string_view> instructionKeys;
        const auto item =
            row.nested("deliveredInstructions." + std::to_string(restart.deliveredInstructions.size())).tracking(instructionKeys);
        restart.deliveredInstructions.push_back(
            {item.get<Id>(instruction, "messageId"), item.get<int>(instruction, "step"), item.get<std::string>(instruction, "text")});
        item.rejectUnknown(instruction);
    }
    row.rejectUnknown(data);
    return restart;
}

TaskTrigger decodeTrigger(const Json& data, const RecordDecoder& record) {
    std::set<std::string_view> keys;
    const auto row = record.tracking(keys);
    const auto id = row.get<Id>(data, "triggerId");
    const auto firedAt = row.get<Seconds>(data, "firedAt");
    const auto kind = row.get<std::string>(data, "triggerKind");
    if (kind == "webhook") {
        auto source = row.get<std::string>(data, "source");
        auto event = row.object(row.require(data, "event"), "event");
        row.rejectUnknown(data);
        return {id, firedAt, WebhookTaskTrigger{std::move(source), std::move(event)}};
    }
    const auto timed = enumValue(timedKinds, kind);
    if (!timed) row.fail("triggerKind", "unknown task trigger");
    const auto target = row.get<Seconds>(data, "targetTime");
    if (row.get<Seconds>(data, "coveredTo") != target) row.fail("coveredTo", "does not match targetTime");
    TimedTaskTrigger trigger{*timed,
                             target,
                             row.get<Seconds>(data, "coveredFrom"),
                             row.get<Id>(data, "occurrenceCount"),
                             row.get<std::string>(data, "timezone"),
                             row.optional<std::string>(data, "label")};
    row.rejectUnknown(data);
    return {id, firedAt, std::move(trigger)};
}
}

std::string_view taskPeerOutcomeName(TaskPeerOutcome outcome) {
    return enumName(peerOutcomes, outcome);
}

Json taskRestartJson(const TaskRestart& restart) {
    Json delivered = Json::array();
    for (const auto& item : restart.deliveredInstructions)
        delivered.push_back({{"messageId", item.messageId}, {"step", item.step}, {"text", item.text}});
    return {{"taskId", restart.taskId}, {"deliveredInstructions", std::move(delivered)}};
}

Json taskTriggerJson(const TaskTrigger& trigger) {
    Json data{{"triggerId", trigger.id}, {"firedAt", trigger.firedAt}};
    if (const auto* timed = std::get_if<TimedTaskTrigger>(&trigger.details)) {
        data.update({{"triggerKind", enumName(timedKinds, timed->kind)},
                     {"targetTime", timed->targetTime},
                     {"coveredFrom", timed->coveredFrom},
                     {"coveredTo", timed->targetTime},
                     {"occurrenceCount", timed->occurrenceCount},
                     {"timezone", timed->timezone}});
        if (timed->label) data["label"] = *timed->label;
    } else {
        const auto& webhook = std::get<WebhookTaskTrigger>(trigger.details);
        data.update({{"triggerKind", "webhook"}, {"source", webhook.source}, {"event", webhook.event}});
    }
    return data;
}

Json sourceResultsJson(std::span<const OutboxSource> sources) {
    Json result = Json::array();
    for (const auto& source : sources) result.push_back(sourceJson(source));
    return result;
}

void decodeTaskContext(TaskData& task, const Json& data, const RecordDecoder& row) {
    task.sourceMessageId = row.optional<Id>(data, "sourceMessageId");
    task.sourceTaskId = row.optional<Id>(data, "sourceTaskId");
    task.sourceMessageCoversFullRequest = row.optional<bool>(data, "sourceMessageCoversFullRequest");
    task.requester = row.optional<std::string>(data, "requester");
    if (const auto found = row.find(data, "sourceResultRefs"); found != data.end())
        task.sourceResultRefs = taskResultReferences(*found, task.id);
    if (const auto found = row.find(data, "restartContext"); found != data.end())
        task.restartContext = decodeRestart(*found, row.nested("restartContext"));
    if (const auto found = row.find(data, "trigger"); found != data.end()) task.trigger = decodeTrigger(*found, row.nested("trigger"));
    if (const auto workflow = row.optional<Id>(data, "workflowId"))
        task.workflow = TaskWorkflow{*workflow, row.get<std::string>(data, "stageId"), row.get<std::string>(data, "inputArtifactVersion")};
    if (const auto peer = row.optional<std::string>(data, "peerOutcome")) {
        task.peerOutcome = enumValue(peerOutcomes, *peer);
        if (!task.peerOutcome) row.fail("peerOutcome", "unknown peer outcome");
    }
}

void encodeTaskContext(Json& data, const TaskData& task) {
    if (task.sourceMessageId) data["sourceMessageId"] = *task.sourceMessageId;
    if (task.sourceTaskId) data["sourceTaskId"] = *task.sourceTaskId;
    if (task.sourceMessageCoversFullRequest) data["sourceMessageCoversFullRequest"] = *task.sourceMessageCoversFullRequest;
    if (task.requester) data["requester"] = *task.requester;
    if (!task.sourceResultRefs.empty() || task.workflow) data["sourceResultRefs"] = sourceResultsJson(task.sourceResultRefs);
    if (task.restartContext) data["restartContext"] = taskRestartJson(*task.restartContext);
    if (task.trigger) data["trigger"] = taskTriggerJson(*task.trigger);
    if (const auto& workflow = task.workflow) {
        data["workflowId"] = workflow->id;
        data["stageId"] = workflow->stageId;
        data["inputArtifactVersion"] = workflow->inputArtifactVersion;
    }
    if (task.peerOutcome) data["peerOutcome"] = taskPeerOutcomeName(*task.peerOutcome);
}
}
