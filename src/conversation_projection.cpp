#include "conversation_storage.hpp"

#include <map>
#include <vector>

namespace flockv0::conversation_storage {
namespace {
// The production bridge embeds the context JSON in an input_text string.
constexpr std::size_t toolHistoryWireBytes = 768 * 1024;
constexpr std::size_t olderLargeResultBytes = 16 * 1024;
constexpr std::size_t webResultWindowBytes = 40 * 1024;
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

struct ResultSize {
    std::size_t full, compact;
    bool rereadable, windowed;

    std::size_t extra() const { return full - std::min(full,compact); }
    bool large() const { return rereadable && extra() > largeResultBytes; }
};
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
    std::vector<ResultSize> sizes;
    sizes.reserve(turn.size());
    std::size_t used = 2;
    for (auto& entry : turn) {
        entry["step"] = entry.at("_ref").at("step");
        entry["callIndex"] = entry.at("_ref").at("callIndex");
        auto compact = omitted(entry);
        entry.erase("_ref");
        ResultSize size{wireBytes(entry),wireBytes(compact),compact.at("result").contains("resultRef"),false};
        // Preserve the first result separately as the evidence anchor.
        size.windowed = !sizes.empty() && size.large() && entry.at("tool") == "web_fetch";
        used += size.compact+1;
        sizes.push_back(size);
        projection.push_back(std::move(compact));
    }

    const auto latestStep = turn.back().at("step").get<Id>();
    std::size_t windowBegin = 0, windowBytes = 0, latestWebBytes = 0;
    for (std::size_t begin = 0; begin < turn.size();) {
        const auto step = turn.at(begin).at("step").get<Id>();
        auto end = begin;
        std::size_t batchBytes = 0;
        while (end < turn.size() && turn.at(end).at("step") == step) {
            if (sizes[end].windowed) batchBytes += sizes[end].extra();
            ++end;
        }
        // Start a new window at batch boundaries so a new result does not evict one body at a time.
        if (batchBytes && batchBytes > webResultWindowBytes - std::min(windowBytes,webResultWindowBytes)) {
            windowBegin = begin;
            windowBytes = 0;
        }
        windowBytes += batchBytes;
        if (step == latestStep) latestWebBytes = batchBytes;
        begin = end;
    }

    auto remainingOlderResults = olderLargeResultBytes;
    const auto keepFull = [&](std::size_t index, std::size_t& remaining) {
        auto& full = turn.at(index);
        auto& compact = projection.at(index);
        const auto& size = sizes[index];
        const auto base = used-size.compact;
        if (size.full > toolHistoryWireBytes || base > toolHistoryWireBytes-size.full) return;
        if (full.at("step") != latestStep && size.large()) {
            if (size.extra() > remaining) return;
            remaining -= size.extra();
        }
        used = base+size.full;
        compact = std::move(full);
    };
    auto latest = turn.size();
    while (latest && turn.at(latest-1).at("step") == latestStep) keepFull(--latest,remainingOlderResults);
    if (latest) keepFull(0,remainingOlderResults);
    for (auto older = latest; older > 1;) {
        --older;
        if (!sizes[older].windowed) keepFull(older,remainingOlderResults);
    }
    auto remainingWebWindow = webResultWindowBytes - std::min(latestWebBytes,webResultWindowBytes);
    for (auto index = std::max(windowBegin,std::size_t{1}); index < latest; ++index)
        if (sizes[index].windowed) keepFull(index,remainingWebWindow);
    turn = std::move(projection);
}
}
