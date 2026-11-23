#include "conversation_storage.hpp"

#include <map>

namespace flockv0::conversation_storage {
namespace {
// The production bridge embeds the context JSON in an input_text string.
constexpr std::size_t toolHistoryWireBytes = 768 * 1024;
constexpr std::size_t olderLargeResultBytes = 16 * 1024;
constexpr std::size_t largeResultBytes = 4 * 1024;

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
    for (const auto* key : {"ok","notRun","untrustedData","accessRevoked","unsupported","cursorStale","stale",
                           "applied","taskId","waitId","nextOffset","nextCursor","truncated"}) {
        const auto value = source.find(key);
        if (value != source.end() && (value->is_boolean() || value->is_number_integer()))
            result[key] = *value;
    }
    if (const auto cursor = field(source,"nextCursor"); !cursor.empty() && cursor.size() <= 1024)
        result["nextCursor"] = cursor;
    for (const auto* key : {"delivery","effect","error"}) {
        const auto value = field(source,key);
        if (!value.empty()) result[key] = bounded(value,128);
    }
    if (const auto data = source.find("data"); data != source.end() && data->is_object()) {
        // Keep small identifiers and pagination fields alongside the saved result reference.
        auto preview = Json::object();
        std::size_t remaining = 1024;
        for (const auto& [key,value] : data->items()) {
            if (!value.is_primitive() || key.size() > 256 ||
                (value.is_string() && value.get_ref<const std::string&>().size() > 256)) continue;
            const auto bytes = wireBytes(Json{{key,value}});
            if (bytes > std::min(remaining,std::size_t{256})) continue;
            preview[key] = value;
            remaining -= bytes;
        }
        if (!preview.empty()) result["data"] = std::move(preview);
    }
    if (canRereadToolResult(tool)) result["resultRef"] = entry.at("_ref");
    else if (tool == "read_tool_result" && source.value("ok",false))
        result["resultRef"] = entry.at("args");
    Json compact{{"tool",tool},{"step",entry.at("step")},{"callIndex",entry.at("callIndex")},{"result",std::move(result)}};
    if (wireBytes(entry.at("args")) <= (tool == "expand_context" ? 4096u : 1024u)) {
        compact["args"] = entry.at("args");
        return compact;
    }
    compact["args"] = Json::object();
    if (!entry.at("args").empty()) compact["argumentsOmitted"] = true;
    return compact;
}
}

void projectToolHistory(Json& turn, const Json& redactionRevision) {
    if (turn.empty()) return;
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
    const auto latestStep = turn.back().at("step").get<Id>();
    auto remainingOlderResults = olderLargeResultBytes;
    const auto keepFull = [&](std::size_t index) {
        auto& full = turn.at(index);
        auto& compact = projection.at(index);
        full.erase("_ref");
        const auto compactBytes = wireBytes(compact);
        const auto bytes = wireBytes(full);
        const auto base = used-compactBytes;
        if (bytes > toolHistoryWireBytes || base > toolHistoryWireBytes-bytes) return;
        if (full.at("step") != latestStep && compact.at("result").contains("resultRef") &&
            bytes > compactBytes + largeResultBytes) {
            if (bytes-compactBytes > remainingOlderResults) return;
            remainingOlderResults -= bytes-compactBytes;
        }
        used = base+bytes;
        compact = std::move(full);
    };
    // Keep the current batch first, then the initial result that may anchor a chain of reads.
    auto older = turn.size();
    while (older && turn.at(older-1).at("step") == latestStep) keepFull(--older);
    if (older) keepFull(0);
    while (older > 1) keepFull(--older);
    turn = std::move(projection);
}
}
