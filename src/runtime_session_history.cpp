#include "runtime_session_history.hpp"
#include "runtime_task_updates.hpp"
#include "flock/limits.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/model_error.hpp"
#include "flock/t3.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
struct HistoryUnavailable : ModelError {
    using ModelError::ModelError;
};
}

Runtime::Components::SessionHistory::SessionHistory(Store::Impl& db, T3& t3, const Policy& policy, TaskUpdates& updates,
                                                    const std::string& mode)
    : db(db), t3(t3), policy(policy), updates(updates), mode(mode) {}

std::size_t Runtime::Components::SessionHistory::readOffset(const Json& args) {
    if (args.contains("offset") && (!args["offset"].is_number_integer() || args["offset"].get<Id>() < 0))
        throw Conflict("Invalid history offset");
    return args.value("offset", std::size_t{0});
}

SessionExcerptEntry Runtime::Components::SessionHistory::readExcerpt(std::string_view bot, const ReadRequest& req) {
    const auto scrub = db.secretStorage.redactor(bot);
    const auto excerpt = [&] {
        try {
            return t3.read(req);
        } catch (const ModelError& error) {
            throw HistoryUnavailable(scrub.text(error.what()), error.transient);
        } catch (const std::exception& error) {
            throw HistoryUnavailable(scrub.text(error.what()), false);
        }
    }();
    if ((!excerpt.threadId.empty() && excerpt.threadId != req.threadId) || (!excerpt.runId.empty() && excerpt.runId != req.runId))
        throw Conflict("History source does not match the requested Run");
    scrub.requirePublic({excerpt.itemId, excerpt.nextCursor});
    const auto redacted = scrub.text(excerpt.text);
    auto text = bounded(redacted, limits::session_evidence::maxHistoryExcerptBytes);
    const bool clipped = text.size() < redacted.size();
    return {req.environmentId,
            req.threadId,
            req.runId,
            excerpt.itemId,
            std::move(text),
            excerpt.nextCursor,
            excerpt.nextOffset,
            excerpt.more || clipped,
            excerpt.nextOffset > req.offset || clipped};
}

bool Runtime::Components::SessionHistory::readEvidence(const TaskData& task, const Json& event, const Json& args, const Json& usage) {
    ReadRequest req;
    req.environmentId = task.environmentId;
    req.gatewayEnvironmentId = task.gatewayEnvironmentId.value_or("");
    req.config = task.config;
    req.threadId = field(args, "threadId");
    req.runId = field(args, "runId");
    req.itemId = field(args, "itemId");
    req.cursor = field(args, "cursor");
    req.view = field(args, "view").empty() ? "messages" : field(args, "view");
    req.offset = readOffset(args);
    if (req.view != "messages" && req.view != "activity") throw Conflict("Unsupported history view");
    const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
    const auto active = runRef(report.value("run", Json::object()));
    if (active.threadId == req.threadId && (active.runId == req.runId || field(report, "resultRunId") == req.runId)) {
        req.environmentId = report.at("environmentId");
        req.gatewayEnvironmentId = active.gatewayEnvironmentId;
        if (req.gatewayEnvironmentId.empty() && !task.hasDestinations()) req.gatewayEnvironmentId = task.gatewayEnvironmentId.value_or("");
    } else {
        std::lock_guard lock(db.mutex);
        auto row = db.query("SELECT DISTINCT environment_id FROM runs WHERE task_id=? AND thread_id=? AND run_id=? LIMIT 2",
                            {task.id, req.threadId, req.runId});
        if (!row.next()) throw Conflict("History reference does not belong to this task");
        req.environmentId = row.string(0);
        if (row.next()) throw Conflict("History reference matches more than one Session");
    }
    if (req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
    if (!policy.allows(task.botId, Resource::sessionRead(req.environmentId, req.threadId))) throw Conflict("Session read access required");
    auto entry = encodeSessionExcerpt(readExcerpt(task.botId, req));
    return updates.change(task, [&](TaskData& current) {
        recordSessionRead(current, event, std::move(entry), usage);
        return true;
    });
}

void Runtime::Components::SessionHistory::appendEventEvidence(const TaskData& task, const Json& event, const Json& entry) {
    auto evidence = event.at("evidence");
    if (evidence.size() >= maxEvidenceReads) throw Conflict("History read allowance exhausted");
    evidence.push_back(entry);
    db.exec("UPDATE events SET evidence=? WHERE id=? AND task_id=?", {evidence, event["id"], task.id});
}

void Runtime::Components::SessionHistory::recordSessionRead(const TaskData& task, const Json& event, Json entry, const Json& usage) {
    appendEventEvidence(task, event, entry);
    if (!usage.empty()) entry["usage"] = usage;
    db.journalStorage.journal(task.id, "session_read", entry);
}

Runtime::Components::SessionHistory::PrefillResult Runtime::Components::SessionHistory::prefillHistory(const TaskData& task,
                                                                                                       const Json& event) {
    if (mode != "prefill" || !event["evidence"].empty() || (event["kind"] != "result" && !event.contains("sessionResult")))
        return PrefillResult::NotNeeded;
    const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
    const auto run = runRef(report["run"]);
    if (!policy.allows(task.botId, Resource::sessionRead(report.value("environmentId", task.environmentId), run.threadId)))
        return PrefillResult::NotNeeded;
    try {
        readEvidence(task, event, {{"threadId", run.threadId}, {"runId", run.runId}, {"view", "activity"}});
        return PrefillResult::Persisted;
    } catch (const HistoryUnavailable&) {
        return PrefillResult::UnavailableWithSettledReport;
    }
}
}
