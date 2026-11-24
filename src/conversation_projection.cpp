#include "conversation_storage.hpp"

#include <map>

namespace flockv0::conversation_storage {
namespace {
// The production bridge embeds the context JSON in an input_text string.
constexpr std::size_t toolHistoryWireBytes = 768 * 1024;

std::size_t wireBytes(const Json& value) {
    const auto encoded = value.dump(-1,' ',false,Json::error_handler_t::replace);
    // dump already escaped control bytes; the outer string escapes its quotes and backslashes.
    return encoded.size()+std::ranges::count_if(encoded,[](char c) { return c=='"' || c=='\\'; });
}

using CallRef = std::pair<Id,Id>;
CallRef callRef(const Json& data) {
    return {integer(data,"step"),integer(data,"callIndex")};
}

Json omitted(const Json& entry) {
    const auto& source = entry.at("result");
    const auto& tool = entry.at("tool").get_ref<const std::string&>();
    Json result{{"evidenceOmitted",true}};
    for (const auto* key : {"ok","notRun","untrustedData","accessRevoked","unsupported","cursorStale","stale","applied","taskId","waitId"}) {
        const auto value = source.find(key);
        if (value != source.end() && (value->is_boolean() || value->is_number_integer()))
            result[key] = *value;
    }
    for (const auto* key : {"delivery","error"}) {
        const auto value = field(source,key);
        if (!value.empty()) result[key] = bounded(value,128);
    }
    if (canRereadToolResult(tool)) result["resultRef"] = entry.at("_ref");
    Json compact{{"tool",tool},{"step",entry.at("step")},{"callIndex",entry.at("callIndex")},{"result",std::move(result)}};
    if (tool == "expand_context" && wireBytes(entry.at("args")) <= 4096) {
        compact["args"] = entry.at("args");
        return compact;
    }
    compact["args"] = Json::object();
    if (!entry.at("args").empty()) compact["argumentsOmitted"] = true;
    return compact;
}
}

void projectToolHistory(Json& turn, const Json& redactionRevision) {
    std::map<CallRef,const Json*> sources;
    for (auto& entry : turn) {
        const auto& tool = entry.at("tool");
        auto& result = entry.at("result");
        if (tool == "expand_context") {
            if (result.value("ok",false) && (!entry.contains("_redactionRevision") ||
                                             entry.at("_redactionRevision") != redactionRevision)) {
                result = {{"ok",false},{"evidenceOmitted",true},{"cursorStale",true},
                          {"error","Redaction changed. Start a new expand_context read."}};
            }
            entry.erase("_redactionRevision");
        } else if (tool == "read_tool_result") {
            if (!result.value("ok",false)) continue;
            const auto source = sources.find(callRef(entry.at("args")));
            if (source == sources.end()) {
                result = toolFailure("Tool result is unavailable");
                continue;
            }
            const auto& record = *source->second;
            result = {{"ok",true},{"tool",record.at("tool")},{"args",record.at("args")},
                      {"result",record.at("result")}};
            if (record.at("result").value("untrustedData",false)) result["untrustedData"] = true;
        } else {
            sources.emplace(callRef(entry.at("_ref")),&entry);
        }
    }

    auto projection = Json::array();
    std::size_t used = 2;
    for (auto& entry : turn) {
        entry["step"] = entry.at("_ref").at("step");
        entry["callIndex"] = entry.at("_ref").at("callIndex");
        auto compact = omitted(entry);
        used += wireBytes(compact)+1;
        projection.push_back(std::move(compact));
    }
    for (std::size_t i = turn.size(); i > 0; --i) {
        auto& full = turn.at(i-1);
        auto& compact = projection.at(i-1);
        full.erase("_ref");
        const auto base = used-wireBytes(compact);
        if (const auto bytes = wireBytes(full); bytes <= toolHistoryWireBytes &&
            base <= toolHistoryWireBytes-bytes) {
            used = base+bytes;
            compact = std::move(full);
        }
    }
    turn = std::move(projection);
}
}
