#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "database.hpp"
#include "run_ref_storage.hpp"
#include "flock/secrets.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
namespace {
using namespace std::literals;

constexpr std::array commandTools{
    std::pair{CommandTool::delegate, "delegate"sv},
    std::pair{CommandTool::start_in_session, "start_in_session"sv},
    std::pair{CommandTool::follow_up, "follow_up"sv},
    std::pair{CommandTool::answer_session_question, "answer_session_question"sv},
    std::pair{CommandTool::message_session, "message_session"sv},
};
constexpr std::array outboxPhases{
    std::pair{OutboxPhase::ready, "ready"sv},           std::pair{OutboxPhase::started, "started"sv},
    std::pair{OutboxPhase::accepted, "accepted"sv},     std::pair{OutboxPhase::closed, "closed"sv},
    std::pair{OutboxPhase::superseded, "superseded"sv},
};

template <class T> void writeOptional(Json& object, std::string_view key, const std::optional<T>& value) {
    if (value) object[key] = *value;
}

Json nullableId(std::optional<Id> id) {
    return id ? Json(*id) : Json(nullptr);
}

OutboxSourceRun readSourceRun(const Json& data, const RecordDecoder& row) {
    return {.threadId = row.get<std::string>(data, "threadId"),
            .runId = row.get<std::string>(data, "runId"),
            .environmentId = row.optional<std::string>(data, "environmentId"),
            .gatewayEnvironmentId = row.optional<std::string>(data, "gatewayEnvironmentId"),
            .link = row.optional<std::string>(data, "link"),
            .itemId = row.optional<std::string>(data, "itemId"),
            .status = row.optional<std::string>(data, "status"),
            .assignment = decodeOptionalRunAssignment(data, row)};
}

OutboxSource readSource(const Json& data, const RecordDecoder& row) {
    OutboxSource source{.taskId = row.get<Id>(data, "taskId"),
                        .messageId = row.get<Id>(data, "messageId"),
                        .revisionId = row.optional<Id>(data, "revisionId"),
                        .text = row.optional<std::string>(data, "text"),
                        .resultRunId = row.optional<std::string>(data, "resultRunId"),
                        .run = std::nullopt};
    if (const auto run = row.find(data, "run"); run != data.end()) source.run = decodeOutboxSourceRun(*run, row.nested("run"));
    return source;
}

OutboxInstruction readInstruction(const Json& data, const RecordDecoder& row) {
    OutboxInstruction item{.messageId = row.get<Id>(data, "messageId"),
                           .step = row.get<int>(data, "step"),
                           .text = row.get<std::string>(data, "text"),
                           .revisionId = row.nullable<Id>(data, "revisionId"),
                           .conflicting = row.get<bool>(data, "conflicting"),
                           .source = std::nullopt};
    if (const auto source = row.find(data, "source"); source != data.end()) item.source = decodeOutboxSource(*source, row.nested("source"));
    return item;
}

template <class T> std::vector<T> readArray(const Json& data, const RecordDecoder& row, T (*decode)(const Json&, const RecordDecoder&)) {
    std::vector<T> items;
    for (const auto& item : row.array(data)) items.push_back(row.nested(std::to_string(items.size())).decodeClosed(item, decode));
    return items;
}

OutboxContext readContext(const Json& data, const RecordDecoder& row) {
    row.object(data);
    return {row.optional<Id>(data, "answerEventId"), row.optional<Id>(data, "sourceMessageId"),
            row.optional<std::string>(data, "peerMessageId")};
}

Json contextJson(const OutboxContext& context) {
    Json result = Json::object();
    writeOptional(result, "answerEventId", context.answerEventId);
    writeOptional(result, "peerMessageId", context.peerMessageId);
    writeOptional(result, "sourceMessageId", context.sourceMessageId);
    return result;
}

Json readAnswers(const Json& data, const RecordDecoder& row) {
    for (const auto& [field, answer] : row.object(data).items()) {
        if (answer.is_string()) continue;
        const auto path = row.nested(field);
        for (const auto& choice : path.array(answer)) {
            if (!choice.is_string()) path.fail({}, "expected an array of strings");
        }
    }
    return data;
}

OutboxStopApproval readStopApproval(const Json& data, const RecordDecoder& row) {
    const auto& policy = row.require(data, "stopPolicy");
    const auto& origin = row.require(data, "stopPolicyOrigin");
    const auto policyRow = row.nested("stopPolicy");
    const auto originRow = row.nested("stopPolicyOrigin");
    auto tool = policyRow.get<std::string>(policy, "tool");
    if (tool != "cancel_task") policyRow.fail("tool", "unknown stop approval tool");
    return {{std::move(tool), policyRow.object(policyRow.require(policy, "args"), "args"), {}},
            policyRow.get<Id>(policy, "generation"),
            policyRow.get<Id>(policy, "revisionId"),
            originRow.get<std::string>(origin, "scope"),
            originRow.get<std::string>(origin, "requester"),
            originRow.get<Id>(origin, "anchor"),
            originRow.get<Id>(origin, "turn"),
            originRow.get<bool>(origin, "background")};
}

std::optional<OutboxStop> readStop(const Json& data, const RecordDecoder& row, std::optional<std::string> operation) {
    const bool hasRun = data.contains("stopThreadId") || data.contains("stopRunId");
    const bool hasApproval = data.contains("stopPolicy") || data.contains("stopPolicyOrigin");
    if (!operation) {
        if (hasRun || hasApproval) row.fail("stopOperation", "required for a stop request");
        return std::nullopt;
    }
    OutboxStop stop{std::move(*operation), std::nullopt, std::nullopt};
    if (hasRun) stop.run = OutboxRunIdentity{row.get<std::string>(data, "stopThreadId"), row.get<std::string>(data, "stopRunId")};
    if (hasApproval) stop.approval = readStopApproval(data, row);
    return stop;
}

void writeStopPayload(Json& data, const OutboxStop& stop) {
    if (stop.run) {
        data["stopRunId"] = stop.run->runId;
        data["stopThreadId"] = stop.run->threadId;
    }
    if (const auto& approval = stop.approval) {
        data["stopPolicy"] = stopPolicyJson(*approval);
        data["stopPolicyOrigin"] = {{"scope", approval->scope},
                                    {"requester", approval->requester},
                                    {"anchor", approval->anchor},
                                    {"turn", approval->turn},
                                    {"background", approval->background}};
    }
}

constexpr std::string_view outboxColumns = "tool,environment_id,project_id,revision_id,session_participation_id,stop_operation,"
                                           "instruction,instruction_deferred,memory_version,memory_section,memory_excerpt_bytes,"
                                           "role_hash,role_section,role_excerpt_bytes,data";
constexpr std::string_view outboxBindings = "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
const auto outboxUpdate = "UPDATE outbox SET (" + std::string(outboxColumns) + ")=" + std::string(outboxBindings);
}

std::string_view commandToolName(CommandTool tool) {
    return tool == CommandTool::none ? std::string_view{} : enumName(commandTools, tool);
}

std::optional<CommandTool> commandToolFromName(std::string_view name) {
    return enumValue(commandTools, name);
}

std::string_view outboxPhaseName(OutboxPhase phase) {
    return enumName(outboxPhases, phase);
}

Json stopPolicyJson(const OutboxStopApproval& approval) {
    return {{"tool", approval.decision.tool},
            {"args", approval.decision.args},
            {"generation", approval.generation},
            {"revisionId", approval.revisionId}};
}

Json sourceRunJson(const OutboxSourceRun& run) {
    Json data = {{"threadId", run.threadId}, {"runId", run.runId}};
    writeOptional(data, "environmentId", run.environmentId);
    writeOptional(data, "gatewayEnvironmentId", run.gatewayEnvironmentId);
    writeOptional(data, "link", run.link);
    writeOptional(data, "itemId", run.itemId);
    writeOptional(data, "status", run.status);
    if (run.assignment) encodeRunAssignment(data, *run.assignment);
    return data;
}

Json sourceJson(const OutboxSource& source) {
    Json data = {{"taskId", source.taskId}, {"messageId", source.messageId}};
    writeOptional(data, "revisionId", source.revisionId);
    writeOptional(data, "text", source.text);
    writeOptional(data, "resultRunId", source.resultRunId);
    if (source.run) data["run"] = sourceRunJson(*source.run);
    return data;
}

Json instructionJson(const OutboxInstruction& item) {
    Json data = {{"messageId", item.messageId},
                 {"step", item.step},
                 {"text", item.text},
                 {"revisionId", nullableId(item.revisionId)},
                 {"conflicting", item.conflicting}};
    if (item.source) data["source"] = sourceJson(*item.source);
    return data;
}

OutboxInstruction decodeOutboxInstruction(const Json& data, const RecordDecoder& row) {
    return row.decodeClosed(data, readInstruction);
}

void redactSource(OutboxSource& source, const SecretRedactor& scrub) {
    const auto redact = [&](std::optional<std::string>& value) {
        if (value) *value = scrub.text(*value);
    };
    redact(source.text);
    redact(source.resultRunId);
    if (auto& run = source.run) {
        run->threadId = scrub.text(run->threadId);
        run->runId = scrub.text(run->runId);
        redact(run->environmentId);
        redact(run->gatewayEnvironmentId);
        redact(run->link);
        redact(run->itemId);
        redact(run->status);
        if (auto& assignment = run->assignment) {
            assignment->ownerBotId = scrub.text(assignment->ownerBotId);
            assignment->projectId = scrub.text(assignment->projectId);
        }
    }
}

void redactInstructions(std::span<OutboxInstruction> instructions, const SecretRedactor& scrub) {
    for (auto& instruction : instructions) {
        instruction.text = scrub.text(instruction.text);
        if (instruction.source) redactSource(*instruction.source, scrub);
    }
}

Json instructionsJson(std::span<const OutboxInstruction> instructions) {
    auto data = Json::array();
    for (const auto& instruction : instructions) data.push_back(instructionJson(instruction));
    return data;
}

std::vector<OutboxInstruction> taskInstructions(const Json& data, Id task, std::string_view key) {
    const auto id = std::to_string(task);
    return readArray(data, {"tasks", id, std::string(key)}, readInstruction);
}

std::vector<OutboxSource> taskResultReferences(const Json& data, Id task) {
    const auto id = std::to_string(task);
    return readArray(data, {"tasks", id, "sourceResultRefs"}, readSource);
}

OutboxSource decodeOutboxSource(const Json& data, const RecordDecoder& row) {
    return row.decodeClosed(data, readSource);
}

OutboxSourceRun decodeOutboxSourceRun(const Json& data, const RecordDecoder& row) {
    return row.decodeClosed(data, readSourceRun);
}

namespace {
Json outboxPayload(const OutboxAction& outbox) {
    const auto& command = outbox.command;
    Json data = {{"target", command.target.threadId.empty() ? Json(nullptr) : runJson(command.target)},
                 {"questionId", command.questionId},
                 {"answers", command.answers},
                 {"instructions", Json::array()},
                 {"contextIds", contextJson(outbox.contextIds)},
                 {"sourceResultRefs", Json::array()}};
    if (!command.gatewayEnvironmentId.empty()) data["gatewayEnvironmentId"] = command.gatewayEnvironmentId;
    if (outbox.destinationAction) data["destinationAction"] = true;
    if (outbox.commit) {
        data["expectedCommit"] = outbox.commit->expected;
        data["markedCommitChecks"] = outbox.commit->markedChecks;
    }
    if (command.receipt) data["receipt"] = runJson(*command.receipt);
    if (outbox.stop) writeStopPayload(data, *outbox.stop);
    for (const auto& item : outbox.instructions) data["instructions"].push_back(instructionJson(item));
    for (const auto& source : outbox.sourceResultRefs) data["sourceResultRefs"].push_back(sourceJson(source));
    return data;
}
}

Json outboxActionJson(const OutboxAction& outbox) {
    const auto& command = outbox.command;
    auto data = outboxPayload(outbox);
    data["tool"] = commandToolName(command.tool);
    data["environmentId"] = command.environmentId;
    data["projectId"] = command.projectId;
    data["revisionId"] = nullableId(outbox.revisionId);
    if (outbox.instructionBase)
        data["instructionBase"] = *outbox.instructionBase;
    else
        data["instruction"] = command.instruction;
    writeOptional(data, "sessionParticipationId", outbox.sessionParticipationId);
    if (outbox.stop) {
        data["stopRequested"] = true;
        data["stopOperation"] = outbox.stop->operation;
    }
    return data;
}

Json outboxColumnValues(const Outbox& outbox) {
    const auto& command = outbox.command;
    const auto& [role, memory] = outbox.sessionContext;
    return Json::array({commandToolName(command.tool), command.environmentId, command.projectId, nullableId(outbox.revisionId),
                        nullableId(outbox.sessionParticipationId), outbox.stop ? Json(outbox.stop->operation) : Json(nullptr),
                        outbox.instructionBase ? *outbox.instructionBase : command.instruction,
                        static_cast<int>(outbox.instructionBase.has_value()), memory ? Json(memory->version) : Json(nullptr),
                        memory ? Json(memory->section) : Json(nullptr), memory ? Json(memory->excerptBytes) : Json(nullptr),
                        role ? Json(role->version) : Json(nullptr), role ? Json(role->section) : Json(nullptr),
                        role ? Json(role->excerptBytes) : Json(nullptr), outboxPayload(outbox)});
}

Outbox decodeOutboxRow(const Statement& columns) {
    const auto id = columns.string(0);
    const RecordDecoder row("outbox", id);
    const auto storedPhase = enumValue(outboxPhases, columns.string(1));
    if (!storedPhase) row.fail("phase", "unknown outbox phase");
    const auto data = row.parse(columns.string(17));
    row.object(data);

    const auto tool = commandToolFromName(columns.string(3));
    if (!tool || *tool == CommandTool::message_session) row.fail("tool", "unknown outbox tool");
    Outbox outbox;
    auto& command = outbox.command;
    command.actionId = id;
    command.tool = *tool;
    command.environmentId = columns.string(4);
    command.projectId = columns.string(5);
    outbox.authGeneration = columns.string(2);
    command.gatewayEnvironmentId = row.optional<std::string>(data, "gatewayEnvironmentId").value_or("");
    command.questionId = row.get<std::string>(data, "questionId");
    command.answers = readAnswers(row.require(data, "answers"), row.nested("answers"));
    const auto& target = row.require(data, "target");
    if (!target.is_null()) command.target = decodeRunRef(target, row.nested("target"));
    if (const auto receipt = data.find("receipt"); receipt != data.end()) command.receipt = decodeRunRef(*receipt, row.nested("receipt"));

    outbox.phase = *storedPhase;
    if (!columns.null(6)) outbox.revisionId = columns.number(6);
    if (columns.number(10)) {
        if (*tool != CommandTool::delegate && *tool != CommandTool::start_in_session)
            row.fail("instructionBase", "unexpected for this outbox tool");
        outbox.instructionBase = columns.string(9);
    } else {
        command.instruction = columns.string(9);
    }
    outbox.instructions = readArray(row.require(data, "instructions"), row.nested("instructions"), readInstruction);
    outbox.contextIds = readContext(row.require(data, "contextIds"), row.nested("contextIds"));
    outbox.sourceResultRefs = readArray(row.require(data, "sourceResultRefs"), row.nested("sourceResultRefs"), readSource);
    outbox.destinationAction = row.optional<bool>(data, "destinationAction").value_or(false);
    if (data.contains("expectedCommit") || data.contains("markedCommitChecks"))
        outbox.commit = OutboxCommit{row.get<std::string>(data, "expectedCommit"), row.get<bool>(data, "markedCommitChecks")};
    if (!columns.null(7)) outbox.sessionParticipationId = columns.number(7);
    if (!columns.null(11)) {
        const auto excerptBytes = columns.number(13);
        if (!std::in_range<std::size_t>(excerptBytes)) row.fail("sessionMemory.excerptBytes", "integer out of range");
        outbox.sessionContext.memory = SessionMemory{columns.number(11), columns.string(12), static_cast<std::size_t>(excerptBytes)};
    }
    if (!columns.null(14)) {
        const auto excerptBytes = columns.number(16);
        if (!std::in_range<std::size_t>(excerptBytes)) row.fail("sessionRole.excerptBytes", "integer out of range");
        outbox.sessionContext.role = SessionRole{columns.string(14), columns.string(15), static_cast<std::size_t>(excerptBytes)};
    }
    outbox.stop = readStop(data, row, columns.null(8) ? std::nullopt : std::optional{columns.string(8)});
    return outbox;
}

std::optional<Outbox> OutboxStorage::outbox(const TaskData& task, std::string_view actionId) const {
    const auto id = actionId.empty() ? task.actionId : std::string(actionId);
    if (id.empty()) return std::nullopt;
    static const auto sql = "SELECT id,phase,auth_generation," + std::string(outboxColumns) + " FROM outbox WHERE id=? AND task_id=?";
    auto row = db_.query(sql.c_str(), {id, task.id});
    if (!row.next()) return std::nullopt;
    return decodeOutboxRow(row);
}

void OutboxStorage::insertOutbox(Id task, const Outbox& action) {
    static const auto sql = "INSERT INTO outbox(" + std::string(outboxColumns) +
                            ",id,task_id,phase,auth_generation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    auto values = outboxColumnValues(action);
    values.push_back(action.command.actionId);
    values.push_back(task);
    values.push_back(outboxPhaseName(action.phase));
    values.push_back(action.authGeneration);
    db_.exec(sql.c_str(), values);
}

bool OutboxStorage::saveOutbox(const Outbox& action, std::optional<OutboxPhase> expected) {
    static const auto sql = outboxUpdate + " WHERE id=? AND phase=coalesce(?,phase)";
    auto values = outboxColumnValues(action);
    values.push_back(action.command.actionId);
    values.push_back(expected ? Json(outboxPhaseName(*expected)) : Json(nullptr));
    db_.exec(sql.c_str(), values);
    return sqlite3_changes(db_.db) == 1;
}

bool OutboxStorage::setOutboxPhase(std::string_view id, OutboxPhase phase, std::optional<OutboxPhase> expected) {
    if (expected)
        db_.exec("UPDATE outbox SET phase=? WHERE id=? AND phase=?", {outboxPhaseName(phase), id, outboxPhaseName(*expected)});
    else
        db_.exec("UPDATE outbox SET phase=? WHERE id=?", {outboxPhaseName(phase), id});
    return sqlite3_changes(db_.db) == 1;
}

bool OutboxStorage::startOutbox(const Outbox& action, std::string_view authGeneration) {
    static const auto sql = outboxUpdate + ",phase='started',auth_generation=? WHERE id=? AND phase='ready'";
    auto values = outboxColumnValues(action);
    values.push_back(authGeneration);
    values.push_back(action.command.actionId);
    db_.exec(sql.c_str(), values);
    return sqlite3_changes(db_.db) == 1;
}

bool OutboxStorage::prepareOutbox(const Outbox& action) {
    static const auto sql = outboxUpdate + " WHERE id=? AND phase='ready' AND instruction_deferred=1";
    auto values = outboxColumnValues(action);
    values.push_back(action.command.actionId);
    db_.exec(sql.c_str(), values);
    return sqlite3_changes(db_.db) == 1;
}
}
