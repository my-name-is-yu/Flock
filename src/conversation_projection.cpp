#include "conversation_storage.hpp"

#include <map>
#include <set>

namespace flockv0::conversation_storage {
namespace {
// The production bridge embeds the context JSON in an input_text string.
constexpr std::size_t toolHistoryWireBytes = 768 * 1024;
// Leave room for multiple escaped result chunks beyond the latest batch.
constexpr std::size_t earlierEvidenceWireBytes = 40 * 1024;

std::size_t wireBytes(const Json& value) {
    const auto encoded = value.dump(-1,' ',false,Json::error_handler_t::replace);
    // dump already escaped control bytes; the outer string escapes its quotes and backslashes.
    return encoded.size()+std::ranges::count_if(encoded,[](char c) { return c=='"' || c=='\\'; });
}

using CallRef = std::pair<Id,Id>;
CallRef callRef(const Json& data) {
    return {integer(data,"step"),integer(data,"callIndex")};
}

void copyContinuation(Json& summary, const Json& source, const char* key) {
    const auto value = source.find(key);
    if (value == source.end()) return;
    if (value->is_null() || value->is_number_integer() ||
        (value->is_string() && wireBytes(*value) <= 1024))
        summary["continuation"][key] = *value;
}

Json resultSummary(const Json& entry) {
    const auto& source = entry.at("result");
    const auto& tool = entry.at("tool").get_ref<const std::string&>();
    auto summary = Json::object();
    for (const auto* key : {"ok","notRun","untrustedData","accessRevoked","unsupported","cursorStale","stale",
                           "applied","taskId","waitId","truncated"}) {
        const auto value = source.find(key);
        if (value != source.end() && (value->is_boolean() || value->is_number_integer())) summary[key] = *value;
    }
    for (const auto* key : {"delivery","effect","error"}) {
        const auto value = field(source,key);
        if (!value.empty()) summary[key] = bounded(value,128);
    }
    if (tool == "read_result") copyContinuation(summary,source,"nextOffset");
    if (tool == "expand_context" || tool == "flock_session_search" || tool == "flock_session_events")
        copyContinuation(summary,source,"nextCursor");
    if (tool == "git_find_repos") copyContinuation(summary,source,"nextPage");
    if (const auto data = source.find("data"); data != source.end() && data->is_object()) {
        if (tool == "mcp_discover") copyContinuation(summary,*data,"nextCursor");
        if (tool == "git_manage_issue") copyContinuation(summary,*data,"nextPage");
    }
    return summary;
}

struct ToolHistoryEntry {
    const Json& call;
    const Json& source;

    const Json* savedRef() const {
        return canRereadToolResult(source.at("tool").get_ref<const std::string&>()) ? &source.at("_ref") : nullptr;
    }

    Json project(bool full) const {
        auto result = full ? source.at("result") : resultSummary(source);
        const bool reopened = &source != &call;
        if (reopened) {
            const bool untrusted = result.value("untrustedData",false);
            result = {{"ok",true},{"tool",source.at("tool")},{"result",std::move(result)}};
            if (full) result["args"] = source.at("args");
            if (untrusted) result["untrustedData"] = true;
        }
        if (const auto ref = savedRef(); ref && (reopened || !full)) result["resultRef"] = *ref;
        if (!full) result["evidenceOmitted"] = true;

        const auto& ref = call.at("_ref");
        Json entry{{"tool",call.at("tool")},{"step",ref.at("step")},{"callIndex",ref.at("callIndex")},
                   {"result",std::move(result)}};
        const auto& args = call.at("args");
        if (full || wireBytes(args) <= (call.at("tool") == "expand_context" ? 4096u : 1024u)) {
            entry["args"] = args;
        } else {
            entry["args"] = Json::object();
            entry["argumentsOmitted"] = true;
        }
        return entry;
    }
};
}

void projectToolHistory(Json& turn, const Json& redactionRevision) {
    if (turn.empty()) return;
    std::map<CallRef,const Json*> sources;
    std::vector<ToolHistoryEntry> entries;
    entries.reserve(turn.size());
    auto projection = Json::array();
    std::size_t used = 2;
    for (auto& call : turn) {
        const auto& tool = call.at("tool").get_ref<const std::string&>();
        auto& result = call.at("result");
        const auto* source = &call;
        if (tool == "expand_context" && result.value("ok",false) &&
            (!call.contains("_redactionRevision") || call.at("_redactionRevision") != redactionRevision)) {
            result = {{"ok",false},{"evidenceOmitted",true},{"cursorStale",true},
                      {"error","Redaction changed. Start a new expand_context read."}};
        } else if (tool == "read_tool_result" && result.value("ok",false)) {
            const auto saved = sources.find(callRef(call.at("args")));
            if (saved == sources.end()) result = toolFailure("Tool result is unavailable");
            else source = saved->second;
        } else if (canRereadToolResult(tool)) {
            sources.emplace(callRef(call.at("_ref")),&call);
        }
        const auto& entry = entries.emplace_back(ToolHistoryEntry{call,*source});
        auto compact = entry.project(false);
        used += wireBytes(compact)+1;
        projection.push_back(std::move(compact));
    }

    const auto& latestStep = turn.back().at("_ref").at("step");
    auto remainingEarlier = earlierEvidenceWireBytes;
    std::set<CallRef> seen;
    // Rank saved evidence by most recent call or reopen. At most one row carries the full saved result.
    for (std::size_t i = entries.size(); i > 0; --i) {
        const auto& entry = entries.at(i-1);
        const auto ref = entry.savedRef();
        if (ref && !seen.insert(callRef(*ref)).second) continue;
        auto& compact = projection.at(i-1);
        auto full = entry.project(true);
        const auto compactBytes = wireBytes(compact);
        const auto base = used-compactBytes;
        const auto bytes = wireBytes(full);
        if (bytes > toolHistoryWireBytes || base > toolHistoryWireBytes-bytes) continue;
        if (ref && full.at("step") != latestStep && bytes > compactBytes) {
            const auto extra = bytes-compactBytes;
            if (extra > remainingEarlier) continue;
            remainingEarlier -= extra;
        }
        used = base+bytes;
        compact = std::move(full);
    }
    turn = std::move(projection);
}
}
