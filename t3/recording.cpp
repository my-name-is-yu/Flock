#include "internal.hpp"
#include "flock/detail/traffic_recording.hpp"
#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0::t3detail {
namespace {

using recording::Fields;
using recording::projectArray;
using recording::redactedShape;

enum class Call { Initialize, Launch, Wait, Read };

Json projectRequest(const Json& arguments, Call call, recording::Ids& ids) {
    Fields fields(arguments);
    if (call == Call::Initialize) {
        fields.literal("protocolVersion", {ProtocolVersion});
        return fields.take();
    }
    fields.ids(ids, "environment", {"environmentId"});
    if (call == Call::Launch) {
        fields.ids(ids, "project", {"projectId"});
        fields.ids(ids, "action", {"clientRequestId"});
        fields.redact({"message", "title"});
    } else {
        fields.ids(ids, "thread", {"threadId"});
        if (call == Call::Wait) {
            fields.ids(ids, "run", {"runId"});
            fields.counters({"timeoutMs"});
            fields.redact({"untilDelegatedWorkSettled"});
        } else {
            fields.ids(ids, "item", {"itemId"});
            fields.literal("view", {"activity", "messages"});
            fields.counters({"limit", "maxCharsPerItem", "runLimit", "afterPosition", "beforePosition", "textOffset"});
        }
    }
    return fields.take();
}

void status(Fields& fields) {
    fields.literal("status", {"cancelled", "completed", "expired", "failed", "interrupted", "queued", "running", "starting", "waiting"});
}

Json projectRun(const Json& value, recording::Ids& ids) {
    Fields fields(value);
    fields.ids(ids, "run", {"runId"});
    fields.counters({"ordinal"});
    status(fields);
    return fields.take();
}

Json projectThread(const Json& value, recording::Ids& ids) {
    Fields fields(value);
    fields.ids(ids, "thread", {"threadId"});
    fields.ids(ids, "project", {"projectId"});
    fields.ids(ids, "run", {"activeRunId", "latestRunId"});
    fields.literal("createdBy", {"agent", "user"});
    fields.literal("creationSource", {"mcp", "server", "web"});
    fields.redact({"archived", "link", "title"});
    fields.counters({"pendingRequestCount", "runCount"});
    status(fields);
    return fields.take();
}

Json projectItem(const Json& value, recording::Ids& ids) {
    Fields fields(value);
    fields.ids(ids, "item", {"itemId", "messageId"});
    fields.ids(ids, "run", {"runId"});
    fields.ids(ids, "thread", {"sourceThreadId"});
    fields.literal("type", {"assistant_message", "checkpoint", "command_execution", "error", "notification", "subagent",
                            "user_input_request", "user_message"});
    fields.literal("visibility", {"local", "inherited"});
    fields.literal("creationSource", {"mcp", "provider", "server", "web"});
    fields.literal("createdBy", {"agent", "user"});
    fields.redact({"text", "textTruncated", "title"});
    fields.counters({"position", "nextTextOffset"});
    status(fields);
    return fields.take();
}

Json projectToolError(const Json& value) {
    Fields fields(value);
    fields.literal("code",
                   {"invalid_request", "invalid_params", "thread_not_found", "run_not_found", "item_not_found", "attachment_not_found",
                    "project_not_found", "orchestration_error", "capability_denied", "thread_credential_required",
                    "runtime_mode_escalation_denied", "interaction_mode_escalation_denied", "provider_unavailable", "model_unavailable"});
    fields.literal("message", {"The thread was not found.", "The project was not found."});
    return fields.take();
}

Json projectPayload(const Json& value, Call call, bool isError, recording::Ids& ids) {
    if (isError) {
        auto error = projectToolError(value);
        if (const auto nested = value.find("error"); nested != value.end()) error["error"] = projectToolError(*nested);
        return error;
    }
    Fields fields(value);
    switch (call) {
    case Call::Initialize: fields.literal("protocolVersion", {ProtocolVersion}); break;
    case Call::Launch:
        fields.ids(ids, "project", {"projectId"});
        fields.ids(ids, "thread", {"threadId"});
        fields.ids(ids, "run", {"runId"});
        fields.redact({"link"});
        break;
    case Call::Wait:
        fields.ids(ids, "thread", {"threadId"});
        fields.ids(ids, "run", {"runId"});
        fields.redact({"timedOut"});
        status(fields);
        fields.pick("resultRun", [&](const Json& run) { return projectRun(run, ids); });
        break;
    case Call::Read:
        fields.pick("thread", [&](const Json& thread) { return projectThread(thread, ids); });
        fields.pick("recentRuns",
                    [&](const Json& runs) { return projectArray(runs, [&](const Json& run) { return projectRun(run, ids); }); });
        fields.pick("items",
                    [&](const Json& items) { return projectArray(items, [&](const Json& item) { return projectItem(item, ids); }); });
        fields.redact({"hasMore"});
        fields.counters({"nextPosition", "earlierPosition"});
        break;
    }
    return fields.take();
}

Json projectResult(const Json& value, Call call, recording::Ids& ids) {
    if (call == Call::Initialize) return projectPayload(value, call, false, ids);
    const auto isError = value.find("isError");
    const auto payload = [&](const Json& data) { return projectPayload(data, call, isError != value.end() && *isError == true, ids); };
    Fields fields(value);
    fields.redact({"isError"});
    fields.pick("structuredContent", payload);
    fields.pick("content", [&](const Json& content) {
        return projectArray(content, [&](const Json& item) {
            Fields block(item);
            block.literal("type", {"text"});
            block.pick("text", [&](const Json& data) {
                if (str(item, "type") != "text" || !data.is_string()) return redactedShape(data);
                const auto parsed = Json::parse(data.get_ref<const std::string&>(), nullptr, false);
                return parsed.is_discarded() ? redactedShape(data) : Json(payload(parsed).dump());
            });
            return block.take();
        });
    });
    return fields.take();
}

}

void Client::record(recording::Capture& capture, unsigned id, std::string_view method, const Json& arguments, const Json& message,
                    std::string_view contentType, long status) noexcept {
    try {
        auto call = Call::Initialize;
        std::string tool;
        if (method == "initialize") {
            if (const auto result = message.find("result"); result != message.end()) {
                if (const auto info = result->find("serverInfo"); info != result->end()) recordingVersion_ = str(*info, "version");
            }
        } else {
            if (method != "tools/call") return;
            tool = str(arguments, "name");
            if (tool == "t3_thread_launch")
                call = Call::Launch;
            else if (tool == "t3_thread_wait")
                call = Call::Wait;
            else if (tool == "t3_thread_read")
                call = Call::Read;
            else
                return;
        }
        capture.append(recordingVersion_, [&](recording::Ids& ids) {
            Fields response(message);
            response.literal("jsonrpc", {"2.0"});
            response.counters({"id"});
            response.pick("result", [&](const Json& value) { return projectResult(value, call, ids); });
            response.pick("error", [](const Json& value) {
                Fields error(value);
                error.counters({"code"});
                error.redact({"message"});
                return error.take();
            });
            auto result = response.take();
            auto params = call == Call::Initialize
                              ? projectRequest(arguments, call, ids)
                              : Json{{"name", tool}, {"arguments", projectRequest(arguments.at("arguments"), call, ids)}};
            return Json{{"contentType", contentType.starts_with("text/event-stream") ? "text/event-stream" : "application/json"},
                        {"status", status},
                        {"request", {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}},
                        {"response", std::move(result)}};
        });
    } catch (...) {
        std::fputs("T3 traffic recording refused\n", stderr);
    }
}

}
