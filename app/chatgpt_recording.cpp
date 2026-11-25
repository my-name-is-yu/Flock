#include "chatgpt_recording.hpp"
#include "chatgpt.hpp"
#include "flock/detail/traffic_recording.hpp"
#include "flock/json.hpp"
#include "flock/limits.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0::app::chatgpt {
namespace {

using recording::Fields;
using recording::literalOrRedacted;
using recording::projectArray;
using recording::redactedShape;

Json projectUsage(const Json& value) {
    Fields fields(value);
    fields.counters({"input_tokens", "output_tokens", "total_tokens"});
    fields.pick("input_tokens_details", [](const Json& details) {
        Fields counts(details);
        counts.counters({"cached_tokens"});
        return counts.take();
    });
    return fields.take();
}

Json errorCode(const Json& value) {
    if (std::ranges::any_of(rejectedImageCodes, [&](std::string_view code) { return value == code; })) return value;
    return literalOrRedacted(value, {"invalid_request_error", "invalid_api_key", "unsupported_value", "feature_unavailable", "server_error",
                                     "rate_limit_exceeded", "service_unavailable", "subscription_sharing_usage_limit_exceeded"});
}

void projectErrorFields(Fields& fields) {
    fields.pick("code", errorCode);
    fields.literal("param", {"model", "reasoning", "reasoning.effort", "input"});
}

void projectError(Fields& fields) {
    projectErrorFields(fields);
    fields.pick("error", [](const Json& error) {
        if (!error.is_object()) return errorCode(error);
        Fields nested(error);
        projectErrorFields(nested);
        return nested.take();
    });
}

const Json& declaredFunction(const Json& request, const Json& name) {
    static const Json unavailable;
    const auto& namespaces = field(request, "tools");
    if (namespaces.is_array() && namespaces.size() == 1 && field(namespaces.front(), "name") == "flock") {
        const auto& tools = field(namespaces.front(), "tools");
        if (tools.is_array() && name.is_string()) {
            const auto tool = std::ranges::find_if(tools, [&](const Json& entry) { return field(entry, "name") == name; });
            if (tool != tools.end()) return *tool;
        }
    }
    return unavailable;
}

Json projectArgument(const Json& value, const Json& schema, recording::Ids& ids, std::string_view key = {}) {
    if (value.is_object()) {
        auto result = Json::object();
        const auto& fields = field(schema, "properties");
        for (const auto& [name, child] : value.items()) {
            if (const auto property = fields.find(name); property != fields.end())
                result[name] = projectArgument(child, *property, ids, name);
            else if (fields.is_object())
                result["redacted_field"] = nullptr;
        }
        return result;
    }
    if (value.is_array()) {
        auto result = Json::array();
        for (const auto& child : value) result.push_back(projectArgument(child, field(schema, "items"), ids));
        return result;
    }
    const auto& allowed = field(schema, "enum");
    if (allowed.is_array() && std::ranges::find(allowed, value) != allowed.end()) return value;
    if (key == "threadId" || key == "runId" || key == "itemId" || key == "cursor" || key == "environmentId" || key == "projectId" ||
        key == "sessionRef" || key == "eventRef" || key == "attachmentRef" || key == "messageId" || key == "taskId" || key == "before")
        return ids.replace(key, value);
    if ((key == "limit" || key == "offset") && value.is_number_integer()) return value;
    return redactedShape(value);
}

Json projectArguments(const Json& value, const Json& schema, recording::Ids& ids) {
    if (!value.is_string()) return redactedShape(value);
    const auto& raw = value.get_ref<const std::string&>();
    const auto parseDepth = [](int depth, Json::parse_event_t, Json&) {
        if (depth > limits::model::maxToolArgumentDepth) throw std::invalid_argument("Recorded tool arguments are nested too deeply");
        return true;
    };
    if (raw.empty()) return "";
    if (raw.size() > limits::model::maxToolArgumentBytes) return "{";
    const auto args = Json::parse(raw, parseDepth, false);
    return args.is_discarded() ? "{" : projectArgument(args, schema, ids).dump();
}

Json projectItem(const Json& value, const Json& request, recording::Ids& ids) {
    Fields fields(value);
    fields.literal("type", {"function_call", "reasoning", "message"});
    fields.literal("status", {"in_progress", "completed", "incomplete"});
    fields.literal("role", {"assistant"});
    fields.literal("namespace", {"flock"});
    fields.ids(ids, "item", {"id"});
    fields.ids(ids, "call", {"call_id"});
    const auto& declared = declaredFunction(request, field(value, "name"));
    fields.pick("name", [&](const Json& name) { return declared.is_null() ? redactedShape(name) : name; });
    fields.pick("arguments", [&](const Json& args) { return projectArguments(args, field(declared, "parameters"), ids); });
    return fields.take();
}

Json projectResponse(const Json& value, const Json& request, recording::Ids& ids) {
    Fields fields(value);
    fields.ids(ids, "response", {"id"});
    fields.literal("status", {"queued", "in_progress", "completed", "incomplete", "failed", "cancelled"});
    fields.pick("usage", projectUsage);
    projectError(fields);
    fields.pick("output", [&](const Json& output) {
        return projectArray(output, [&](const Json& item) { return projectItem(item, request, ids); });
    });
    return fields.take();
}

Json projectEvent(const Json& value, const Json& request, recording::Ids& ids) {
    Fields fields(value);
    fields.literal("type", {"response.created", "response.in_progress", "response.completed", "response.incomplete", "response.failed",
                            "response.output_item.added", "response.output_item.done", "response.function_call_arguments.delta",
                            "response.function_call_arguments.done", "response.content_part.added", "response.content_part.done",
                            "response.output_text.delta", "response.output_text.done", "error"});
    fields.counters({"sequence_number", "output_index", "content_index"});
    fields.ids(ids, "item", {"item_id"});
    fields.ids(ids, "response", {"response_id"});
    fields.pick("item", [&](const Json& item) { return projectItem(item, request, ids); });
    fields.pick("response", [&](const Json& response) { return projectResponse(response, request, ids); });
    fields.pick("usage", projectUsage);
    fields.redact({"arguments", "delta"});
    projectError(fields);
    return fields.take();
}

std::string_view modelSlug(const Json& request) {
    const auto& model = field(request, "model");
    return model.is_string() ? model.get_ref<const std::string&>() : std::string_view{};
}

}

ResponseRecording::ResponseRecording(recording::Capture& capture, const Json& request) : capture_(capture), request_(request) {}

ResponseRecording::~ResponseRecording() {
    if (events_.empty() || collectionFailed_) return;
    capture_.append(modelSlug(request_), [this](recording::Ids& ids) {
        auto events = Json::array();
        for (const auto& frame : events_) events.push_back(projectEvent(frame, request_, ids));
        return Json{{"status", status_}, {"contentType", "text/event-stream"}, {"events", std::move(events)}};
    });
}

void ResponseRecording::event(const Json& value, long status) noexcept {
    if (collectionFailed_) return;
    try {
        events_.push_back(value);
        status_ = status;
    } catch (...) {
        collectionFailed_ = true;
    }
}

void ResponseRecording::httpError(const HttpResponse& response) noexcept {
    capture_.append(modelSlug(request_), [&](recording::Ids&) {
        const auto body = Json::parse(response.body, nullptr, false);
        if (body.is_discarded())
            return Json{{"status", response.status},
                        {"contentType", response.contentType.empty() ? "" : "text/plain"},
                        {"body", response.body.empty() ? "" : "redacted"}};
        Fields fields(body);
        projectError(fields);
        return Json{{"status", response.status}, {"contentType", "application/json"}, {"body", fields.take().dump()}};
    });
}

}
