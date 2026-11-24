#include "conversation_storage.hpp"

#include <map>
#include <set>
#include <vector>

namespace flockv0::conversation_storage {
namespace {
// The production bridge embeds the context JSON in an input_text string.
constexpr std::size_t toolHistoryWireBytes = 768 * 1024;
constexpr std::size_t webResultWindowBytes = 192 * 1024;
constexpr std::size_t webAnchorBytes = 16 * 1024;
constexpr std::size_t largeWebResultBytes = 4 * 1024;

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
    if (tool == "find_tasks" || tool == "list_schedules" || tool == "memory_history")
        copyContinuation(summary,source,"before");
    if (tool == "memory_history") copyContinuation(summary,source,"proposalsBefore");
    if (tool == "expand_context" || tool == "flock_session_search" || tool == "flock_session_events")
        copyContinuation(summary,source,"nextCursor");
    if (tool == "git_find_repos") copyContinuation(summary,source,"nextPage");
    if (const auto data = source.find("data"); data != source.end() && data->is_object()) {
        if (tool == "mcp_discover") copyContinuation(summary,*data,"nextCursor");
        if (tool == "git_manage_issue") copyContinuation(summary,*data,"nextPage");
    }
    return summary;
}

struct EarlierEvidenceBudget {
    bool retain(bool reopened, std::size_t extraBytes) {
        if (reopened && reopenSlots > 0) {
            --reopenSlots;
            return true;
        }
        if (!reopened && precedingSlot) {
            precedingSlot = false;
            return true;
        }
        if (extraBytes > sharedBytes) return false;
        sharedBytes -= extraBytes;
        return true;
    }

private:
    std::size_t reopenSlots = 2;
    bool precedingSlot = true;
    std::size_t sharedBytes = 8 * 1024;
};

struct ToolHistoryEntry {
    const Json& call;
    const Json& source;

    const Json* savedRef() const {
        return canRereadToolResult(source.at("tool").get_ref<const std::string&>()) ? &source.at("_ref") : nullptr;
    }

    bool reopened() const { return &source != &call; }

    Json project(bool full) const {
        auto result = full ? source.at("result") : resultSummary(source);
        if (reopened()) {
            const bool untrusted = result.value("untrustedData",false);
            result = {{"ok",true},{"tool",source.at("tool")},{"result",std::move(result)}};
            if (full && !source.at("result").value("notRun",false)) result["args"] = source.at("args");
            if (untrusted) result["untrustedData"] = true;
        }
        if (const auto ref = savedRef(); ref && (reopened() || !full)) result["resultRef"] = *ref;
        if (!full) result["evidenceOmitted"] = true;

        const auto& ref = call.at("_ref");
        Json entry{{"tool",call.at("tool")},{"step",ref.at("step")},{"callIndex",ref.at("callIndex")},
                   {"result",std::move(result)}};
        const auto& args = call.at("args");
        if (!call.at("result").value("notRun",false) &&
            (full || wireBytes(args) <= (call.at("tool") == "expand_context" ? 4096u : 1024u))) {
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
        entries.push_back({call,*source});
    }

    struct VisibleEntry {
        const ToolHistoryEntry& entry;
        Json row;
        std::size_t bytes;
        std::size_t webExtraBytes = 0;
        bool canPromoteWeb = false;
    };
    std::vector<VisibleEntry> visible;
    visible.reserve(entries.size());
    std::size_t remaining = toolHistoryWireBytes - 2;
    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
        auto row = entry->project(false);
        const auto bytes = wireBytes(row) + 1;
        if (bytes > remaining) break;
        remaining -= bytes;
        visible.push_back({*entry,std::move(row),bytes});
    }

    const auto& latestStep = turn.back().at("_ref").at("step");
    EarlierEvidenceBudget earlierEvidence;
    std::set<CallRef> seen;
    for (auto& selected : visible) {
        const auto& entry = selected.entry;
        const auto ref = entry.savedRef();
        if (ref && !seen.insert(callRef(*ref)).second) continue;
        auto full = entry.project(true);
        const auto bytes = wireBytes(full) + 1;
        const auto extraBytes = bytes > selected.bytes ? bytes - selected.bytes : 0;
        if (ref && entry.call.at("tool") == "web_fetch" && extraBytes > largeWebResultBytes) {
            selected.webExtraBytes = extraBytes;
            selected.canPromoteWeb = true;
        }
        if (bytes > remaining + selected.bytes) continue;
        if (ref && full.at("step") != latestStep && !earlierEvidence.retain(entry.reopened(),extraBytes)) continue;
        remaining = remaining + selected.bytes - bytes;
        selected.row = std::move(full);
        selected.canPromoteWeb = false;
    }

    const auto isWebAnchor = [&](const VisibleEntry& selected) {
        return &selected.entry == &entries.front() && selected.webExtraBytes > 0 &&
               selected.webExtraBytes <= webAnchorBytes;
    };
    auto webWindow = visible.rbegin();
    std::size_t webBytes = 0;
    for (auto batch = visible.rbegin(); batch != visible.rend();) {
        const auto start = batch;
        const auto& step = start->row.at("step");
        std::size_t batchBytes = 0;
        do {
            if (!isWebAnchor(*batch)) batchBytes += batch->webExtraBytes;
            ++batch;
        } while (batch != visible.rend() && batch->row.at("step") == step);
        if (batchBytes > webResultWindowBytes - std::min(webBytes,webResultWindowBytes)) {
            webWindow = start;
            webBytes = 0;
        }
        webBytes += batchBytes;
    }
    // The ordinary evidence budget has first claim on space for recent and reopened results.
    const auto promoteWeb = [&](VisibleEntry& selected) {
        if (!selected.canPromoteWeb || selected.webExtraBytes > remaining) return;
        remaining -= selected.webExtraBytes;
        selected.row = selected.entry.project(true);
    };
    if (!visible.empty() && isWebAnchor(visible.back())) promoteWeb(visible.back());
    if (webBytes <= webResultWindowBytes) {
        for (auto entry = webWindow; entry != visible.rend(); ++entry)
            if (!isWebAnchor(*entry)) promoteWeb(*entry);
    }
    auto projection = Json::array();
    for (auto entry = visible.rbegin(); entry != visible.rend(); ++entry)
        projection.push_back(std::move(entry->row));
    turn = std::move(projection);
}
}
