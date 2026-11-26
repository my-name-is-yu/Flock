#include "views.hpp"
#include "flock/peer_conversations.hpp"
#include "flock/workflows.hpp"
#include "flock/tasks.hpp"
#include "flock/conversations.hpp"
#include "flock/markdown.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/store.hpp"
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
std::string field(const Json& object, std::string_view key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

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

std::string threadSegment(std::string_view id, std::string_view punctuation = "-_.~") {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string segment;
    for (const unsigned char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || punctuation.find(c) != punctuation.npos)
            segment += static_cast<char>(c);
        else {
            segment += '%';
            segment += hex[c >> 4];
            segment += hex[c & 15];
        }
    }
    return segment;
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
    return "/" + threadSegment(environment, punctuation) + "/" + threadSegment(thread, punctuation);
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
        view["imageUrl"] = "/api/bot-avatar?bot=" + threadSegment(botId) + "&v=" + threadSegment(version);
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
    if (wait.allowance) view["allowance"] = *wait.allowance;
    if (reason == WaitReason::user_question || reason == WaitReason::t3_input) view["text"] = wait.text;
    if (reason == WaitReason::user_question) view["choices"] = wait.choices();
    return view;
}

bool containsProtectedText(const Policy& policy, std::string_view bot, std::string_view text) {
    const Json probe = {{"text", std::string(text)}};
    return policy.redactSecrets(bot, probe).at("text") != probe.at("text");
}

Json eventView(const Json& event, const markdown::SensitiveText& sensitive) {
    auto view = Json::object();
    const auto kind = field(event, "kind");
    copyFields(view, event, {"id", "time", "kind"});
    if (kind != "bot_message") copyFields(view, event, {"taskId"});
    const auto& data = event.at("data");
    Json details = Json::object();
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
    else if (kind == "memory_remembered")
        copyFields(details, data, {"text", "revisionId", "version", "canUndo", "undone"});
    else if (kind == "pr_review_requested")
        copyFields(details, data, {"itemId", "title", "text", "prUrl"});
    else if (kind == "completed") {
        copyFields(details, data, {"text", "revisionId", "runOutcome", "requestedCommit"});
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
    } else if (kind == "bot_message") {
        copyFields(details, data, {"exchangeId", "messageId", "fromBotId", "toBotId", "messageType", "requestKind", "text", "replyTo"});
        if (const auto forwarded = data.find("forwardedMessage"); forwarded != data.end() && forwarded->is_object()) {
            const auto text = field(*forwarded, "text");
            const auto time = forwarded->find("time");
            if (!text.empty() && time != forwarded->end() && time->is_number_integer()) {
                details["forwardedMessage"] = {{"text", text}, {"time", *time}};
                if (event.value("forwardedFromViewer", false)) details["forwardedMessage"]["fromViewer"] = true;
            }
        }
    } else if (kind == "report") {
        copyFields(details, data,
                   {"report", "status", "resultRunId", "truncated", "itemId", "revisionId", "destination", "commit", "requestedCommit"});
        if (data.contains("evidenceStatus")) copyFields(details, data, {"evidenceStatus"});
        if (data.contains("evidenceStatus") || data.contains("destination")) copyFields(details, data, {"detail"});
        details["run"] = runView(data.at("run"), field(data, "environmentId"));
    } else if (kind == "conversation_message" || kind == "conversation_reply") {
        copyFields(details, data, {"text", "messageId", "replyToMessageId", "taskId", "route", "status", "deliveredToT3"});
        details["textHtml"] = markdown::toHtml(field(data, "text"), sensitive);
        if (kind == "conversation_message") {
            copyFields(details, data, {"imageAttachmentCount"});
            if (const auto wake = data.find("sessionWake"); wake != data.end() && wake->is_object())
                copyFields(details["sessionWake"] = Json::object(), *wake, {"threadId", "title", "reason", "sourceMessageId"});
            if (const auto invitation = data.find("sessionInvitation"); invitation != data.end() && invitation->is_object())
                copyFields(details["sessionInvitation"] = Json::object(), *invitation, {"threadId", "title", "inviterName", "note"});
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
                copyFields(item, action, {"tool", "taskId", "workflowId", "stageId", "effect"});
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
        const auto wait = kind == "waiting" ? std::optional{Tasks::waitingEvent(*event)} : std::nullopt;
        const bool incoming = kind == "report" || kind == "completed" || kind == "conversation_reply" || kind == "pr_review_requested" ||
                              (wait && (wait->reason() == WaitReason::user_question || wait->reason() == WaitReason::model_failure));
        auto text = wait ? wait->text : field(data, kind == "report" ? "report" : "text");
        if (kind == "bot_message" && text.empty() && data.contains("forwardedMessage")) text = field(data.at("forwardedMessage"), "text");
        if (incoming) {
            if (!latestIncoming) latestIncoming = event->at("id").get<Id>();
            if (!text.empty()) {
                preview = bounded(text, 240);
                break;
            }
        }
        if (request.empty() && (kind == "request" || kind == "conversation_message" || kind == "bot_message")) request = bounded(text, 240);
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
    if (task.currentRevision) result["currentRevision"] = *task.currentRevision;
    result["deadline"] = task.deadline;
    result["createdAt"] = task.createdAt;
    if (const auto& workflow = task.workflow) {
        result["workflowId"] = workflow->id;
        result["stageId"] = workflow->stageId;
        result["inputArtifactVersion"] = workflow->inputArtifactVersion;
    }
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
    if (const auto* verification = wait.verification()) {
        result["deadline"] = verification->deadline;
        const auto saved = std::ranges::find(task.runResults(), verification->actionId, &DestinationRun::actionId);
        if (saved != task.runResults().end()) run = runView(saved->environmentId, saved->ref.threadId, saved->ref.runId, saved->ref.link);
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
    constexpr std::string_view priority[] = {"idle", "queued", "peer_waiting", "waiting", "thinking", "working", "stopping"};
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

Json activity(const std::vector<Tasks::ActiveTask>& activeTasks, std::string_view bot) {
    const auto states = activeTasks | std::views::transform([](const auto& active) { return displayState(active.task); });
    Json result = {{"presence", botPresence(states)},
                   {"waits", Json::array()},
                   {"queued", Json::array()},
                   {"activeSessions", Json::array()},
                   {"parallelTasks", Json::array()}};
    for (const auto& [task, wait, blocker] : activeTasks) {
        if (task.state == TaskState::waiting && wait) result["waits"].push_back(currentWait(task, *wait));
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
                if (activeRun && !field(run, "threadId").empty()) {
                    auto active = run;
                    active["taskId"] = task.id;
                    active["request"] = task.request;
                    active["createdAt"] = task.createdAt;
                    result["activeSessions"].push_back(std::move(active));
                }
                checks.push_back(std::move(run));
            }
            result["parallelTasks"].push_back({{"id", task.id}, {"runs", std::move(checks)}});
        }
        auto active = runView(task.activeRun, task.environmentId);
        if (!active.is_null()) {
            active["taskId"] = task.id;
            active["request"] = task.request;
            active["state"] = displayState(task);
            active["createdAt"] = task.createdAt;
            result["activeSessions"].push_back(std::move(active));
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
    copyFields(view, bot, {"environmentId", "projectId", "model", "reasoningEffort", "modelCallLimit"});
    return view;
}

Json botList(Store& store, const Json& bots) {
    auto result = Json::array();
    if (bots.empty()) return result;
    const auto statesByBot = Tasks(store).activeTaskStatesByBot();
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

Json conversation(Store& store, std::string_view bot, Id before, std::string_view viewer) {
    auto events = Conversations(store).conversation(bot, before, conversationSize + 1, Conversations::ConversationEvents::all, viewer);
    const auto redactor = Policy{store}.secretRedactor(bot);
    for (auto& event : events)
        if (field(event, "kind") == "waiting") event["data"] = waitView(Tasks::waitingEvent(event));
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

Json peerConversation(Store& store, std::string_view bot, std::string_view peer, Id before, Id anchor, std::string_view viewer) {
    const Policy policy{store};
    const markdown::SensitiveText sensitive = [&](std::string_view text) {
        return containsProtectedText(policy, bot, text) || containsProtectedText(policy, peer, text);
    };
    const auto olderSize = anchor ? conversationSize / 2 : conversationSize;
    auto events = PeerConversations(store).peerConversation(bot, peer, anchor ? anchor : before, olderSize + 1, 0, viewer);
    events = policy.redactSecrets(bot, events);
    events = policy.redactSecrets(peer, events);
    const bool more = events.size() > olderSize;
    Json result = {{"messages", Json::array()}, {"hasOlder", more}};
    auto& page = result["messages"];
    for (auto event = events.begin() + (more ? 1 : 0); event != events.end(); ++event) page.push_back(eventView(*event, sensitive));
    if (anchor) {
        result["anchorId"] = anchor;
        auto newer = PeerConversations(store).peerConversation(bot, peer, 0, conversationSize - olderSize, anchor, viewer);
        newer = policy.redactSecrets(bot, newer);
        newer = policy.redactSecrets(peer, newer);
        for (const auto& event : newer) page.push_back(eventView(event, sensitive));
    }
    return result;
}

Json sessions(Store& store, std::string_view bot, Id before) {
    const auto tasks = Tasks(store).sessionTasks(bot, before, sessionsSize + 1);
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

Json botView(Store& store, const Json& bot, const std::vector<TaskView>* changedSessionTasks, std::string_view viewer) {
    const auto id = field(bot, "id");
    auto result = conversation(store, id, 0, viewer);
    const auto activeTasks = Tasks(store).activeTasks(id);
    result.update(activity(activeTasks, id));
    if (changedSessionTasks) {
        result["sessionTasks"] = sessionGroups(*changedSessionTasks);
    } else {
        result.update(sessions(store, id, 0));
    }
    result["workflows"] = Workflows(store).workflows(id);
    result["compactions"] = Conversations(store).compactions(id, 0, 1);
    result["bot"] = botSettings(bot);
    auto& paused = result["pausedConversations"] = Json::array();
    for (const auto& turn : Conversations(store).pausedConversations(id)) {
        Json view = Json::object();
        copyFields(view, turn, {"messageId", "replyToMessageId", "text", "route", "allowance"});
        paused.push_back(std::move(view));
    }
    if (!paused.empty() && (result["presence"] == "idle" || result["presence"] == "queued")) result["presence"] = "waiting";
    return result;
}
} // namespace flockv0::web
