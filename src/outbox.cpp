#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "storage.hpp"

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
                        .stageId = row.optional<std::string>(data, "stageId"),
                        .artifactVersion = row.optional<std::string>(data, "artifactVersion"),
                        .resultRunId = row.optional<std::string>(data, "resultRunId"),
                        .run = std::nullopt};
    if (const auto run = row.find(data, "run"); run != data.end()) source.run = row.nested("run").decodeClosed(*run, readSourceRun);
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

SessionMemory readMemory(const Json& data, const RecordDecoder& row) {
    return {row.get<Id>(data, "version"), row.get<std::string>(data, "section"), row.get<std::size_t>(data, "excerptBytes")};
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

std::optional<OutboxStop> readStop(const Json& data, const RecordDecoder& row) {
    const bool hasRun = data.contains("stopThreadId") || data.contains("stopRunId");
    const bool hasApproval = data.contains("stopPolicy") || data.contains("stopPolicyOrigin");
    if (!row.optional<bool>(data, "stopRequested").value_or(false)) {
        if (hasRun || hasApproval || data.contains("stopOperation")) row.fail("stopRequested", "required for a stop request");
        return std::nullopt;
    }
    OutboxStop stop{row.get<std::string>(data, "stopOperation"), std::nullopt, std::nullopt};
    if (hasRun) stop.run = OutboxRunIdentity{row.get<std::string>(data, "stopThreadId"), row.get<std::string>(data, "stopRunId")};
    if (hasApproval) stop.approval = readStopApproval(data, row);
    return stop;
}

void writeStop(Json& data, const OutboxStop& stop) {
    data["stopRequested"] = true;
    data["stopOperation"] = stop.operation;
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
    writeOptional(data, "stageId", source.stageId);
    writeOptional(data, "artifactVersion", source.artifactVersion);
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

void redactSource(OutboxSource& source, const SecretRedactor& scrub) {
    const auto redact = [&](std::optional<std::string>& value) {
        if (value) *value = scrub.text(*value);
    };
    redact(source.text);
    redact(source.stageId);
    redact(source.artifactVersion);
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

Json outboxActionJson(const OutboxAction& outbox) {
    const auto& command = outbox.command;
    Json data = {{"tool", commandToolName(command.tool)},
                 {"environmentId", command.environmentId},
                 {"projectId", command.projectId},
                 {"target", command.target.threadId.empty() ? Json(nullptr) : runJson(command.target)},
                 {"questionId", command.questionId},
                 {"answers", command.answers},
                 {"revisionId", nullableId(outbox.revisionId)},
                 {"instructions", Json::array()},
                 {"contextIds", contextJson(outbox.contextIds)},
                 {"sourceResultRefs", Json::array()},
                 {"chargesBudget", outbox.chargesBudget}};
    if (outbox.instructionBase)
        data["instructionBase"] = *outbox.instructionBase;
    else
        data["instruction"] = command.instruction;
    if (!command.gatewayEnvironmentId.empty()) data["gatewayEnvironmentId"] = command.gatewayEnvironmentId;
    if (outbox.destinationAction) data["destinationAction"] = true;
    if (outbox.commit) {
        data["expectedCommit"] = outbox.commit->expected;
        data["markedCommitChecks"] = outbox.commit->markedChecks;
    }
    if (command.candidate) data["candidate"] = runJson(*command.candidate);
    if (!command.candidateQuestionId.empty()) data["candidateQuestionId"] = command.candidateQuestionId;
    if (!command.launchReceiptThreadId.empty()) data["launchReceiptThreadId"] = command.launchReceiptThreadId;
    writeOptional(data, "sessionParticipationId", outbox.sessionParticipationId);
    if (outbox.stop) writeStop(data, *outbox.stop);
    for (const auto& item : outbox.instructions) data["instructions"].push_back(instructionJson(item));
    for (const auto& source : outbox.sourceResultRefs) data["sourceResultRefs"].push_back(sourceJson(source));
    return data;
}

Json to_json(const Outbox& outbox) {
    auto data = outboxActionJson(outbox);
    if (const auto& memory = outbox.sessionMemory)
        data["sessionMemory"] = {{"version", memory->version}, {"section", memory->section}, {"excerptBytes", memory->excerptBytes}};
    return data;
}

Outbox from_json(std::string_view id, std::string_view phase, std::string_view authGeneration, std::string_view text) {
    const RecordDecoder row("outbox", id);
    const auto storedPhase = enumValue(outboxPhases, phase);
    if (!storedPhase) row.fail("phase", "unknown outbox phase");
    const auto data = row.parse(text);
    row.object(data);

    const auto tool = commandToolFromName(row.get<std::string>(data, "tool"));
    if (!tool || *tool == CommandTool::message_session) row.fail("tool", "unknown outbox tool");
    Outbox outbox;
    auto& command = outbox.command;
    command.actionId = id;
    command.tool = *tool;
    command.environmentId = row.get<std::string>(data, "environmentId");
    command.projectId = row.get<std::string>(data, "projectId");
    outbox.authGeneration = authGeneration;
    command.gatewayEnvironmentId = row.optional<std::string>(data, "gatewayEnvironmentId").value_or("");
    command.questionId = row.get<std::string>(data, "questionId");
    command.answers = readAnswers(row.require(data, "answers"), row.nested("answers"));
    const auto& target = row.require(data, "target");
    if (!target.is_null()) command.target = decodeRunRef(target, row.nested("target"));
    if (const auto candidate = data.find("candidate"); candidate != data.end())
        command.candidate = decodeRunRef(*candidate, row.nested("candidate"));
    command.candidateQuestionId = row.optional<std::string>(data, "candidateQuestionId").value_or("");
    command.launchReceiptThreadId = row.optional<std::string>(data, "launchReceiptThreadId").value_or("");

    outbox.phase = *storedPhase;
    outbox.revisionId = row.nullable<Id>(data, "revisionId");
    outbox.instructionBase = row.optional<std::string>(data, "instructionBase");
    if (outbox.instructionBase) {
        if (data.contains("instruction")) row.fail("instruction", "unexpected with instructionBase");
        if (*tool != CommandTool::delegate && *tool != CommandTool::start_in_session)
            row.fail("instructionBase", "unexpected for this outbox tool");
    } else {
        command.instruction = row.get<std::string>(data, "instruction");
    }
    outbox.instructions = readArray(row.require(data, "instructions"), row.nested("instructions"), readInstruction);
    outbox.contextIds = readContext(row.require(data, "contextIds"), row.nested("contextIds"));
    outbox.sourceResultRefs = readArray(row.require(data, "sourceResultRefs"), row.nested("sourceResultRefs"), readSource);
    outbox.chargesBudget = row.get<bool>(data, "chargesBudget");
    outbox.destinationAction = row.optional<bool>(data, "destinationAction").value_or(false);
    if (data.contains("expectedCommit") || data.contains("markedCommitChecks"))
        outbox.commit = OutboxCommit{row.get<std::string>(data, "expectedCommit"), row.get<bool>(data, "markedCommitChecks")};
    outbox.sessionParticipationId = row.optional<Id>(data, "sessionParticipationId");
    if (const auto memory = data.find("sessionMemory"); memory != data.end())
        outbox.sessionMemory = readMemory(*memory, row.nested("sessionMemory"));
    outbox.stop = readStop(data, row);
    return outbox;
}

std::optional<Outbox> Store::Impl::outbox(const TaskData& task, std::string_view actionId) const {
    const auto id = actionId.empty() ? task.actionId : std::string(actionId);
    if (id.empty()) return std::nullopt;
    auto row = query("SELECT data,phase,auth_generation FROM outbox WHERE id=? AND task_id=?", {id, task.id});
    if (!row.next()) return std::nullopt;
    return from_json(id, row.string(1), row.string(2), row.string(0));
}

void Store::Impl::insertOutbox(Id task, const Outbox& action) {
    exec("INSERT INTO outbox(id,task_id,phase,auth_generation,data) VALUES(?,?,?,?,?)",
         {action.command.actionId, task, outboxPhaseName(action.phase), action.authGeneration, to_json(action)});
}

bool Store::Impl::saveOutbox(const Outbox& action, std::optional<OutboxPhase> expected) {
    if (expected)
        exec("UPDATE outbox SET data=? WHERE id=? AND phase=?", {to_json(action), action.command.actionId, outboxPhaseName(*expected)});
    else
        exec("UPDATE outbox SET data=? WHERE id=?", {to_json(action), action.command.actionId});
    return sqlite3_changes(db) == 1;
}

bool Store::Impl::setOutboxPhase(std::string_view id, OutboxPhase phase, std::optional<OutboxPhase> expected) {
    if (expected)
        exec("UPDATE outbox SET phase=? WHERE id=? AND phase=?", {outboxPhaseName(phase), id, outboxPhaseName(*expected)});
    else
        exec("UPDATE outbox SET phase=? WHERE id=?", {outboxPhaseName(phase), id});
    return sqlite3_changes(db) == 1;
}

bool Store::Impl::startOutbox(const Outbox& action, std::string_view authGeneration) {
    exec("UPDATE outbox SET phase='started',auth_generation=?,data=? WHERE id=? AND phase='ready'",
         {authGeneration, to_json(action), action.command.actionId});
    return sqlite3_changes(db) == 1;
}

bool Store::Impl::prepareOutbox(const Outbox& action) {
    exec("UPDATE outbox SET data=? WHERE id=? AND phase='ready' AND json_type(data,'$.instruction') IS NULL",
         {to_json(action), action.command.actionId});
    return sqlite3_changes(db) == 1;
}
}
