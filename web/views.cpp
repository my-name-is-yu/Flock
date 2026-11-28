#include "flock/task_history.hpp"
#include "flock/percent_encoding.hpp"
#include "flock/task_list.hpp"
#include "views.hpp"
#include "http.hpp"
#include "flock/conversations.hpp"
#include "flock/markdown.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace flockv0::web {
namespace {
constexpr unsigned conversationSize = 36;
constexpr unsigned sessionsSize = 6;
constexpr auto hostedSessionBase = "https://app.t3.codes/threads";

void copyFields(Json& output, const Json& source, std::initializer_list<const char*> keys) {
    if (!source.is_object()) return;
    for (const auto* key : keys) {
        const auto value = source.find(key);
        if (value != source.end() && (value->is_string() || value->is_boolean() || value->is_number())) output[key] = *value;
    }
}

void copyTrigger(Json& output, const Json& source) {
    const auto trigger = source.find("trigger");
    if (trigger == source.end() || !trigger->is_object()) return;
    copyFields(output["trigger"] = Json::object(), *trigger,
               {"triggerKind", "triggerId", "targetTime", "coveredFrom", "coveredTo", "occurrenceCount", "firedAt", "timezone", "label",
                "source"});
}

bool localThreadLink(std::string_view url, std::string_view path) {
    if (!url.starts_with("http://")) return false;
    url.remove_prefix(7);
    const auto slash = url.find('/');
    if (slash == std::string_view::npos || url.substr(slash) != path) return false;
    const auto server = url.substr(0, slash);
    for (const std::string_view host : {"127.0.0.1", "localhost", "[::1]"}) {
        if (server == host) return true;
        if (!server.starts_with(host) || server.size() <= host.size() + 1 || server[host.size()] != ':') continue;
        const auto port = server.substr(host.size() + 1);
        unsigned number = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
        if (error == std::errc{} && end == port.data() + port.size() && number > 0 && number <= 65535) return true;
    }
    return false;
}

std::string sessionPath(std::string_view environment, std::string_view thread, std::string_view punctuation = "-_.~") {
    if (environment.empty() || thread.empty() || environment == "." || environment == ".." || thread == "." || thread == "..") return {};
    return "/" + percentEncode(environment, punctuation) + "/" + percentEncode(thread, punctuation);
}

std::string hostedSessionPath(std::string_view environment, std::string_view thread) {
    // T3 Code's hosted links also escape dots and tildes.
    return sessionPath(environment, thread, "-_");
}

Json runView(std::string_view environment, std::string_view thread, std::string_view run, const std::string& saved) {
    const auto loopbackPath = sessionPath(environment, thread);
    if (loopbackPath.empty()) return nullptr;
    return {{"threadId", thread},
            {"runId", run},
            {"environmentId", environment},
            {"link", localThreadLink(saved, loopbackPath) ? saved : hostedSessionBase + hostedSessionPath(environment, thread)}};
}

Json runView(const Json& run, std::string_view environment) {
    const auto actualEnvironment = field(run, "environmentId");
    if (!actualEnvironment.empty()) environment = actualEnvironment;
    return runView(environment, field(run, "threadId"), field(run, "runId"), field(run, "link"));
}

Json runView(const std::optional<AssignedRun>& run, std::string_view environment) {
    if (!run) return nullptr;
    if (!run->environmentId.empty()) environment = run->environmentId;
    return runView(environment, run->ref.threadId, run->ref.runId, run->ref.link);
}

Json avatarView(const Json& source, std::string_view botId) {
    Json view = Json::object();
    copyFields(view, source, {"kind", "example", "icon", "color", "eyeColor", "filename", "version"});
    const auto version = field(source, "version");
    if (field(source, "kind") == "upload" && !version.empty() && !botId.empty()) {
        view["imageUrl"] = "/api/bot-avatar?bot=" + percentEncode(botId) + "&v=" + percentEncode(version);
    }
    if (const auto definition = source.find("definition"); definition != source.end() && definition->is_object()) {
        Json shape = Json::object();
        copyFields(shape, *definition, {"version", "id", "name"});
        if (definition->contains("body")) copyFields(shape["body"] = Json::object(), definition->at("body"), {"path"});
        if (definition->contains("eyes"))
            copyFields(shape["eyes"] = Json::object(), definition->at("eyes"), {"x", "y", "spacing", "scale"});
        if (shape.contains("body") && shape.contains("eyes")) view["definition"] = std::move(shape);
    }
    return view;
}

Json botSummary(const Json& bot) {
    const auto id = field(bot, "id");
    const auto name = field(bot, "name");
    Json summary = {{"id", id}, {"name", name.empty() ? id : name}};
    copyFields(summary, bot, {"role"});
    if (const auto avatar = bot.find("avatar"); avatar != bot.end() && avatar->is_object()) summary["avatar"] = avatarView(*avatar, id);
    return summary;
}

Json waitView(const Wait& wait) {
    const auto reason = wait.reason();
    Json view = {{"id", wait.id}, {"reason", waitReasonName(reason)}};
    if (wait.messageId) view["messageId"] = *wait.messageId;
    if (reason != WaitReason::policy_approval) view["text"] = wait.text;
    if (reason == WaitReason::user_question) view["choices"] = wait.choices();
    return view;
}

Json eventView(const Json& event, const markdown::SensitiveText& sensitive) {
    auto view = Json::object();
    const auto kind = field(event, "kind");
    copyFields(view, event, {"id", "time", "kind"});
    copyFields(view, event, {"taskId"});
    const auto& data = event.at("data");
    Json details = Json::object();
    const auto renderMarkdown = [&](const char* key) {
        if (const auto text = field(data, key); !text.empty()) details["textHtml"] = markdown::toHtml(text, sensitive);
    };
    if (kind == "waiting")
        details = data;
    else if (kind == "request") {
        copyFields(details, data, {"text", "source", "messageId"});
        copyTrigger(details, data);
    } else if (kind == "wait_closed")
        copyFields(details, data, {"waitId", "resolution"});
    else if (kind == "cancelled")
        copyFields(details, data, {"request"});
    else if (kind == "user_answer")
        copyFields(details, data, {"text", "waitId", "question"});
    else if (kind == "memory_remembered" || kind == "memory_learned") {
        copyFields(details, data,
                   {"text", "revisionId", "version", "canUndo", "undone", "taskTitle", "requestEventId", "diffSummary", "docId"});
        if (kind == "memory_learned")
            for (const auto* key : {"added", "removed"}) details[key] = data.at(key);
    } else if (kind == "bot_setup_updated") {
        copyFields(details, data, {"undone", "sourceMessageId"});
        details["after"] = data.at("after");
        if (data.at("before").contains("soul")) details["before"] = {{"soul", data.at("before").at("soul")}};
        if (auto avatar = details["after"].find("avatar"); avatar != details["after"].end() && avatar->is_object())
            *avatar = avatarView(*avatar, {});
    } else if (kind == "completed") {
        copyFields(details, data, {"text", "revisionId", "runOutcome", "requestedCommit"});
        renderMarkdown("text");
        if (data.contains("source")) {
            const auto& source = data.at("source");
            auto run = runView(source, field(source, "environmentId"));
            if (!run.is_null()) {
                const auto status = field(source, "status");
                if (!status.empty() && !field(run, "runId").empty()) details["runStatus"] = status;
                copyFields(run, source, {"itemId"});
                details["run"] = std::move(run);
            }
        }
    } else if (kind == "report") {
        copyFields(details, data,
                   {"report", "status", "resultRunId", "truncated", "itemId", "revisionId", "destination", "commit", "requestedCommit"});
        if (data.contains("evidenceStatus")) copyFields(details, data, {"evidenceStatus"});
        if (data.contains("evidenceStatus") || data.contains("destination")) copyFields(details, data, {"detail"});
        details["run"] = runView(data.at("run"), field(data, "environmentId"));
        renderMarkdown("report");
    } else if (kind == "conversation_message" || kind == "conversation_reply") {
        copyFields(
            details, data,
            {"text", "messageId", "replyToMessageId", "taskId", "route", "status", "instructionAcceptedAt", "instructionDeliveredAt"});
        details["textHtml"] = markdown::toHtml(field(data, "text"), sensitive);
        if (kind == "conversation_message") {
            copyFields(details, data, {"attachmentCount", "imageAttachmentCount"});
            if (const auto files = data.find("files"); files != data.end() && !files->empty()) {
                auto& projected = details["files"] = Json::array();
                for (const auto& file : *files) {
                    Json item = Json::object();
                    copyFields(item, file, {"id", "name", "sizeBytes"});
                    const auto preview = conversationMediaKind(field(file, "mimeType"));
                    if (!preview.empty()) item["preview"] = preview;
                    projected.push_back(std::move(item));
                }
            }
            if (const auto wake = data.find("sessionWake"); wake != data.end() && wake->is_object())
                copyFields(details["sessionWake"] = Json::object(), *wake, {"threadId", "title", "reason", "sourceMessageId"});
            if (const auto invitation = data.find("sessionInvitation"); invitation != data.end() && invitation->is_object())
                copyFields(details["sessionInvitation"] = Json::object(), *invitation, {"threadId", "title", "inviterName", "note"});
            if (const auto bot = data.find("botMessage"); bot != data.end() && bot->is_object())
                copyFields(details["botMessage"] = Json::object(), *bot, {"fromBotId", "fromName"});
        }
        if (const auto channel = data.find("channel"); channel != data.end() && channel->is_object())
            copyFields(details["channel"] = Json::object(), *channel, {"channelKind", "fromViewer"});
        for (const auto* key : {"attachments", "artifacts"}) {
            const auto items = data.find(key);
            if (items != data.end() && items->is_array()) details[key] = *items;
        }
        if (const auto actions = data.find("actions"); actions != data.end() && actions->is_array()) {
            details["actions"] = Json::array();
            for (const auto& action : *actions) {
                Json item = Json::object();
                copyFields(item, action, {"tool", "taskId", "effect"});
                details["actions"].push_back(std::move(item));
            }
        }
        if (const auto suggestions = data.find("suggestions"); suggestions != data.end() && suggestions->is_array()) {
            details["suggestions"] = Json::array();
            for (const auto& suggestion : *suggestions)
                if (suggestion.is_string()) details["suggestions"].push_back(suggestion);
        }
    }
    view["data"] = std::move(details);
    return view;
}

void conversationSummary(Json& row, Store& store, std::string_view bot) {
    constexpr unsigned previewSize = 12;
    const auto events = Conversations(store).conversation(bot, 0, previewSize);
    std::string request;
    std::string preview;
    Id latestIncoming = 0;
    for (auto event = events.rbegin(); event != events.rend(); ++event) {
        const auto kind = field(*event, "kind");
        const auto& data = event->at("data");
        const auto wait = kind == "waiting" ? std::optional{TaskHistory::waitingEvent(*event)} : std::nullopt;
        const bool incoming = kind == "report" || kind == "completed" || kind == "conversation_reply" ||
                              (wait && wait->reason() != WaitReason::policy_approval && wait->reason() != WaitReason::t3_input);
        auto text = wait ? wait->text : field(data, kind == "report" ? "report" : "text");
        if (incoming) {
            if (!latestIncoming) latestIncoming = event->at("id").get<Id>();
            if (!text.empty()) {
                preview = bounded(text, 240);
                break;
            }
        }
        if (request.empty() && (kind == "request" || kind == "conversation_message")) request = bounded(text, 240);
    }
    // A burst of outgoing messages can push an unread reply outside the preview page.
    if (!latestIncoming && events.size() == previewSize) {
        const auto earlier =
            Conversations(store).conversation(bot, events.front().at("id").get<Id>(), 1, Conversations::ConversationEvents::incoming);
        if (!earlier.empty()) latestIncoming = earlier.front().at("id").get<Id>();
    }
    row["preview"] = preview.empty() ? std::move(request) : std::move(preview);
    row["latestIncomingEventId"] = latestIncoming;
}

std::string_view displayState(TaskState state, bool replanning = false, bool queuedInT3 = false) {
    if (state == TaskState::cancelling || ((state != TaskState::cancelled && state != TaskState::completed) && replanning))
        return "stopping";
    if (state == TaskState::running && queuedInT3) return "queued";
    return taskStateName(state);
}

std::string_view displayState(const TaskData& task) {
    return displayState(task.state, task.replanning, task.queuedInT3);
}

Json taskSummary(const TaskData& task) {
    Json result = {{"state", displayState(task)}, {"id", task.id}, {"request", task.request}, {"generation", task.generation}};
    if (task.state == TaskState::external_waiting && task.waitingNote) result["waitingNote"] = *task.waitingNote;
    if (task.currentRevision) result["currentRevision"] = *task.currentRevision;
    result["createdAt"] = task.createdAt;
    return result;
}

Json verificationRun(const DestinationRun& source) {
    auto run = runView(source.environmentId, source.ref.threadId, source.ref.runId, source.ref.link);
    if (run.is_null()) run = Json::object();
    run["name"] = source.name;
    run["environmentId"] = source.environmentId;
    if (source.report) run["commit"] = source.report->commit;
    run["state"] = destinationRunStatusName(source.status);
    if (source.inPhase(DestinationRunPhase::confirmation_pending) && !field(run, "runId").empty() && source.detail)
        run["detail"] = *source.detail;
    return run;
}

Json currentWait(const TaskData& task, const Wait& wait) {
    auto result = taskSummary(task);
    const auto& env = task.environmentId;
    auto& view = result["wait"] = waitView(wait);
    auto& run = view["run"];
    if (wait.run) {
        const auto& saved = wait.run->run;
        const auto environment = wait.run->environmentId && !wait.run->environmentId->empty() ? *wait.run->environmentId : env;
        run = runView(environment, saved.threadId, saved.runId, saved.link);
    }
    if (run.is_null()) run = runView(task.lastRun, env);
    return result;
}

Json sessionGroup(const TaskView& view) {
    const auto& task = view.task;
    auto sessions = Json::array();
    const auto& env = task.environmentId;
    const auto& activeRun = task.activeRun;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& entry : view.runs | std::views::reverse) {
        const auto& run = entry.run;
        const auto& environment = run.environmentId.empty() ? env : run.environmentId;
        const auto& thread = run.ref.threadId;
        const auto& runId = run.ref.runId;
        auto session = runView(environment, thread, runId, run.ref.link);
        if (session.is_null()) continue;
        if (!seen.emplace(environment, thread).second) continue;
        const bool active = activeRun && runId == activeRun->ref.runId && thread == activeRun->ref.threadId && environment == env;
        session["state"] = active ? displayState(task) : entry.status.value_or("ended");
        const auto result = std::ranges::find_if(task.runResults(), [&](const DestinationRun& check) {
            return check.environmentId == run.environmentId && check.ref.threadId == thread && check.ref.runId == runId;
        });
        if (result != task.runResults().end()) {
            session["name"] = result->name;
            if (result->report) session["commit"] = result->report->commit;
            session["state"] = destinationRunStatusName(result->status);
        }
        if (entry.sessionCreatedAt) session["createdAt"] = *entry.sessionCreatedAt;
        sessions.push_back(std::move(session));
    }
    auto group = taskSummary(task);
    group["sessions"] = std::move(sessions);
    return group;
}

std::string_view botPresence(const auto& states) {
    constexpr std::string_view priority[] = {"idle", "queued", "waiting", "thinking", "working", "stopping"};
    std::size_t highest = 0;
    for (auto presence : states) {
        if (presence == "running")
            presence = "working";
        else if (presence == "deciding" || presence == "sending")
            presence = "thinking";
        for (std::size_t i = highest + 1; i < std::size(priority); ++i)
            if (presence == priority[i]) highest = i;
    }
    return priority[highest];
}

Json activity(const std::vector<TaskList::ActiveTask>& activeTasks, std::string_view bot) {
    const auto states = activeTasks | std::views::transform([](const auto& active) { return displayState(active.task); });
    Json result = {{"presence", botPresence(states)},
                   {"waits", Json::array()},
                   {"queued", Json::array()},
                   {"activeTasks", Json::array()},
                   {"parallelTasks", Json::array()}};
    for (const auto& [task, wait, blocker] : activeTasks) {
        const bool waiting = task.state == TaskState::waiting && wait.has_value();
        const bool needsAttention = waiting && (wait->reason() == WaitReason::bot_notes_unavailable || wait->reason() == WaitReason::auth ||
                                                wait->reason() == WaitReason::settings);
        if (waiting) result["waits"].push_back(currentWait(task, *wait));
        if (!waiting || needsAttention) {
            auto request = taskSummary(task);
            request["session"] = runView(task.activeRun ? task.activeRun : task.lastRun, task.environmentId);
            if (needsAttention) request["problem"] = waitView(*wait);
            result["activeTasks"].push_back(std::move(request));
        }
        if (task.state == TaskState::queued && blocker) {
            auto queued = taskSummary(task);
            queued["blockedBy"] = Json::object();
            if (blocker->botId == bot)
                queued["blockedBy"] = {
                    {"id", blocker->id}, {"botId", blocker->botId}, {"request", blocker->request}, {"state", displayState(blocker->state)}};
            result["queued"].push_back(std::move(queued));
        }
        if (task.hasDestinations()) {
            Json checks = Json::array();
            for (const auto& target : task.runTargets())
                checks.push_back({{"name", target.name},
                                  {"environmentId", target.route.environmentId},
                                  {"state", task.state == TaskState::cancelling ? "stopping" : "queued"}});
            for (const auto& route : task.runResults()) {
                auto run = verificationRun(route);
                const bool activeRun = route.outstanding();
                if (activeRun && task.state == TaskState::cancelling && !route.inPhase(DestinationRunPhase::confirmation_pending))
                    run["state"] = "stopping";
                checks.push_back(std::move(run));
            }
            result["parallelTasks"].push_back({{"id", task.id}, {"runs", std::move(checks)}});
        }
    }
    return result;
}
} // namespace

std::string externalSessionLink(const Json& run) {
    const auto environment = field(run, "environmentId");
    const auto thread = field(run, "threadId");
    const auto hosted = hostedSessionPath(environment, thread);
    if (hosted.empty()) return {};
    const auto saved = field(run, "link");
    return saved == hostedSessionBase + hosted ? saved : std::string{};
}

Json botSettings(const Json& bot) {
    auto view = botSummary(bot);
    view["soul"] = field(bot, "soul");
    view["fixPullRequests"] = bot.value("fixPullRequests", true);
    copyFields(view, bot, {"environmentId", "projectId", "model", "reasoningEffort"});
    for (const auto* key : {"sessionModel", "workspaceStrategy", "setupChangedBy"})
        if (const auto value = bot.find(key); value != bot.end() && value->is_object()) view[key] = *value;
    return view;
}

void projectMemoryHistoryLinks(Json& result) {
    const auto project = [](Json& revision) {
        const auto learning = revision.find("learning");
        if (learning != revision.end()) learning->at("run") = runView(learning->at("run"), {});
    };
    if (const auto revisions = result.find("revisions"); revisions != result.end()) {
        for (auto& revision : *revisions) project(revision);
    } else {
        project(result);
    }
}

Json botList(Store& store, const Json& bots) {
    auto result = Json::array();
    if (bots.empty()) return result;
    const auto statesByBot = TaskList(store).activeTaskStatesByBot();
    for (const auto& bot : bots) {
        const auto id = field(bot, "id");
        if (id.empty()) continue;
        auto row = botSummary(bot);
        // Keep the full text until the response redacts protected values.
        if (const auto soul = field(bot, "soul"); !soul.empty()) row["about"] = soul;
        conversationSummary(row, store, id);
        const auto states = statesByBot.find(id);
        if (states == statesByBot.end()) {
            row["presence"] = "idle";
        } else {
            row["presence"] = botPresence(
                states->second | std::views::transform([](const TaskPresence& task) { return displayState(task.state, task.replanning); }));
        }
        result.push_back(std::move(row));
    }
    return result;
}

Json conversation(Store& store, std::string_view bot, Id before, const Person* viewer) {
    auto events = Conversations(store).conversation(bot, before, conversationSize + 1, Conversations::ConversationEvents::all, viewer);
    const auto redactor = Policy{store}.secretRedactor(bot);
    for (auto& event : events)
        if (field(event, "kind") == "waiting") event["data"] = waitView(TaskHistory::waitingEvent(event));
    events = redactor.record(std::move(events), SecretRedactor::Fields::external);
    markdown::SensitiveText sensitive;
    if (!redactor.empty()) sensitive = [&](std::string_view text) { return redactor.containsProtectedText(text); };
    const bool more = events.size() > conversationSize;
    Json result = {{"events", Json::array()}, {"olderBefore", 0}};
    auto& page = result["events"];
    for (auto event = events.begin() + (more ? 1 : 0); event != events.end(); ++event) page.push_back(eventView(*event, sensitive));
    if (more) result["olderBefore"] = page.front().at("id");
    return result;
}

Json sessions(Store& store, std::string_view bot, Id before) {
    const auto tasks = TaskHistory(store).finishedTasks(bot, before, sessionsSize + 1);
    Json result = {{"sessionTasks", Json::array()}, {"sessionsBefore", 0}};
    auto& page = result["sessionTasks"];
    for (const auto& task : tasks) {
        if (page.size() == sessionsSize) {
            result["sessionsBefore"] = page.back()["id"];
            break;
        }
        page.push_back(sessionGroup(task));
    }
    return result;
}

Json sessionGroups(const std::vector<TaskView>& tasks) {
    auto groups = Json::array();
    for (const auto& task : tasks) groups.push_back(sessionGroup(task));
    return groups;
}

Json botView(Store& store, const Json& bot, const std::vector<TaskView>* changedSessionTasks, const Person* viewer) {
    const auto id = field(bot, "id");
    auto result = conversation(store, id, 0, viewer);
    const auto activeTasks = TaskList(store).activeTasks(id);
    result.update(activity(activeTasks, id));
    if (changedSessionTasks) {
        result["sessionTasks"] = sessionGroups(*changedSessionTasks);
    } else {
        result.update(sessions(store, id, 0));
    }
    result["compactions"] = Conversations(store).compactions(id, 0, 1);
    result["bot"] = botSettings(bot);
    return result;
}
} // namespace flockv0::web
