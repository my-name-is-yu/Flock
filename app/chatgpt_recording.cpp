#include "chatgpt_recording.hpp"

#include <algorithm>

namespace flockv0::app::chatgpt {
namespace {

using recording::literalOrRedacted;
using recording::redactedShape;

void keep(Json& result, const Json& source, std::string_view key, const auto& project) {
    if (auto it = source.find(key); it != source.end()) result[key] = project(*it);
}

void keepCounts(Json& result, const Json& source, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys)
        keep(result, source, key, [](const Json& value) { return value.is_number() ? value : redactedShape(value); });
}

Json projectUsage(const Json& value) {
    if (!value.is_object()) return redactedShape(value);
    auto result = Json::object();
    keepCounts(result, value, {"input_tokens", "output_tokens", "total_tokens"});
    keep(result, value, "input_tokens_details", [](const Json& details) {
        auto projected = redactedShape(details);
        keepCounts(projected, details, {"cached_tokens"});
        return projected;
    });
    return result;
}

Json errorCode(const Json& value) {
    if (std::ranges::any_of(rejectedImageCodes, [&](std::string_view code) { return value == code; })) return value;
    return literalOrRedacted(value, {"invalid_request_error", "invalid_api_key", "unsupported_value", "feature_unavailable", "server_error",
                                     "rate_limit_exceeded", "service_unavailable", "subscription_sharing_usage_limit_exceeded"});
}

void keepErrorFields(Json& result, const Json& value) {
    keep(result, value, "code", errorCode);
    keep(result, value, "param",
         [](const Json& param) { return literalOrRedacted(param, {"model", "reasoning", "reasoning.effort", "input"}); });
}

void keepError(Json& result, const Json& value) {
    keepErrorFields(result, value);
    keep(result, value, "error", [](const Json& error) {
        if (!error.is_object()) return errorCode(error);
        auto projected = Json::object();
        keepErrorFields(projected, error);
        return projected;
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
        if (depth > 20) throw std::invalid_argument("Recorded tool arguments are nested too deeply");
        return true;
    };
    if (raw.empty()) return "";
    if (raw.size() > 32000) return "{";
    const auto args = Json::parse(raw, parseDepth, false);
    return args.is_discarded() ? "{" : projectArgument(args, schema, ids).dump();
}

Json projectItem(const Json& value, const Json& request, recording::Ids& ids) {
    if (!value.is_object()) return redactedShape(value);
    auto result = Json::object();
    keep(result, value, "type", [](const Json& type) { return literalOrRedacted(type, {"function_call", "reasoning", "message"}); });
    keep(result, value, "status", [](const Json& status) { return literalOrRedacted(status, {"in_progress", "completed", "incomplete"}); });
    keep(result, value, "role", [](const Json& role) { return literalOrRedacted(role, {"assistant"}); });
    keep(result, value, "namespace", [](const Json& name) { return literalOrRedacted(name, {"flock"}); });
    keep(result, value, "id", [&](const Json& id) { return ids.replace("item", id); });
    keep(result, value, "call_id", [&](const Json& id) { return ids.replace("call", id); });
    const auto& declared = declaredFunction(request, field(value, "name"));
    keep(result, value, "name", [&](const Json& name) { return declared.is_null() ? redactedShape(name) : name; });
    keep(result, value, "arguments", [&](const Json& args) { return projectArguments(args, field(declared, "parameters"), ids); });
    return result;
}

Json projectResponse(const Json& value, const Json& request, recording::Ids& ids) {
    if (!value.is_object()) return redactedShape(value);
    auto result = Json::object();
    keep(result, value, "id", [&](const Json& id) { return ids.replace("response", id); });
    keep(result, value, "status", [](const Json& status) {
        return literalOrRedacted(status, {"queued", "in_progress", "completed", "incomplete", "failed", "cancelled"});
    });
    keep(result, value, "usage", projectUsage);
    keepError(result, value);
    keep(result, value, "output", [&](const Json& output) {
        if (!output.is_array()) return redactedShape(output);
        auto items = Json::array();
        for (const auto& item : output) items.push_back(projectItem(item, request, ids));
        return items;
    });
    return result;
}

Json projectEvent(const Json& value, const Json& request, recording::Ids& ids) {
    if (!value.is_object()) return redactedShape(value);
    auto result = Json::object();
    keep(result, value, "type", [](const Json& type) {
        return literalOrRedacted(type, {"response.created", "response.in_progress", "response.completed", "response.incomplete",
                                        "response.failed", "response.output_item.added", "response.output_item.done",
                                        "response.function_call_arguments.delta", "response.function_call_arguments.done",
                                        "response.content_part.added", "response.content_part.done", "response.output_text.delta",
                                        "response.output_text.done", "error"});
    });
    keepCounts(result, value, {"sequence_number", "output_index", "content_index"});
    keep(result, value, "item_id", [&](const Json& id) { return ids.replace("item", id); });
    keep(result, value, "response_id", [&](const Json& id) { return ids.replace("response", id); });
    keep(result, value, "item", [&](const Json& item) { return projectItem(item, request, ids); });
    keep(result, value, "response", [&](const Json& response) { return projectResponse(response, request, ids); });
    keep(result, value, "usage", projectUsage);
    keep(result, value, "arguments", redactedShape);
    keep(result, value, "delta", redactedShape);
    keepError(result, value);
    return result;
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
        return Json{{"status", status_}, {"contentType", hasContentType_ ? "text/event-stream" : ""}, {"events", std::move(events)}};
    });
}

void ResponseRecording::event(const Json& value, const HttpResponse& response) noexcept {
    if (collectionFailed_) return;
    try {
        events_.push_back(value);
        status_ = response.status;
        hasContentType_ = !response.contentType.empty();
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
        auto projected = redactedShape(body);
        keepError(projected, body);
        return Json{{"status", response.status}, {"contentType", "application/json"}, {"body", projected.dump()}};
    });
}

}
