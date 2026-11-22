#include "views.hpp"
#include "flock/bot_profile.hpp"

#include <array>
#include <charconv>
#include <map>
#include <set>

namespace flockv0::web {
std::string field(const Json& object, std::string_view key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

namespace {
constexpr unsigned conversationSize = 36;
constexpr unsigned sessionsSize = 6;

void copyFields(Json& output, const Json& source, std::initializer_list<const char*> keys) {
    if (!source.is_object()) return;
    for (const auto* key : keys) {
        const auto value = source.find(key);
        if (value != source.end() && (value->is_string() || value->is_boolean() || value->is_number()))
            output[key] = *value;
    }
}

// The hosted T3 route percent-encodes each ID separately. The Store supplies the owning environment.
std::string threadSegment(std::string_view id) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string segment;
    for (const unsigned char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
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
    const auto server = url.substr(0,slash);
    for (const std::string_view host : {"127.0.0.1","localhost","[::1]"}) {
        if (server == host) return true;
        if (!server.starts_with(host) || server.size() <= host.size()+1 || server[host.size()] != ':') continue;
        const auto port = server.substr(host.size()+1);
        unsigned number = 0;
        const auto [end,error] = std::from_chars(port.data(),port.data()+port.size(),number);
        if (error == std::errc{} && end == port.data()+port.size() && number > 0 && number <= 65535) return true;
    }
    return false;
}

Json runView(const Json& run, std::string_view environment) {
    const auto actualEnvironment = field(run,"environmentId");
    if (!actualEnvironment.empty()) environment = actualEnvironment;
    const auto thread = field(run,"threadId");
    if (environment.empty() || thread.empty() || environment == "." || environment == ".." || thread == "." || thread == "..")
        return nullptr;
    const auto path = "/" + threadSegment(environment) + "/" + threadSegment(thread);
    const auto saved = field(run,"link");
    return {{"threadId",thread},{"runId",field(run,"runId")},{"environmentId",environment},
            {"link",localThreadLink(saved,path) ? saved : "https://app.t3.codes/threads" + path}};
}

Json avatarView(const Json& source, std::string_view botId) {
    auto view = avatar::appearance(source);
    if (view.is_null()) return view;
    const auto version = field(source,"version");
    if (view["kind"] == "upload" && !version.empty() && !botId.empty()) {
        view["version"] = version;
        view["imageUrl"] = "/api/bot-avatar?bot=" + threadSegment(botId) + "&v=" + threadSegment(version);
    }
    return view;
}

Json botSummary(const Json& bot) {
    const auto id = field(bot,"id");
    const auto name = field(bot,"name");
    Json summary = {{"id",id},{"name",name.empty() ? id : name}};
    copyFields(summary,bot,{"role"});
    if (bot.contains("avatar")) {
        auto appearance = avatarView(bot["avatar"],id);
        if (!appearance.is_null()) summary["avatar"] = std::move(appearance);
    }
    return summary;
}

Json settingsView(const Json& bot) {
    auto view = botSummary(bot);
    view["soul"] = field(bot,"soul");
    auto& cron = view["cron"] = Json::array();
    if (bot.contains("cron") && bot["cron"].is_array()) {
        for (const auto& schedule : bot["cron"]) {
            if (!schedule.is_object()) continue;
            Json row = Json::object();
            copyFields(row,schedule,{"id","schedule","timezone","request"});
            cron.push_back(std::move(row));
        }
    }
    return view;
}

struct WaitCopy {
    std::string_view reason, label, text, operationLabel;
};
constexpr WaitCopy waitCopy[] = {
    {"user_question","Question from the Bot","The Bot is waiting for your answer.",""},
    {"budget","Approval for one more run","This request used all of its runs. Approving lets the Bot start one more run, which can use more model and T3 usage.","Approve one more run"},
    {"model_budget","Approval for another model call","This request reached its model call limit. You can approve one more model call.","Approve one more model call"},
    {"model_failure","Retry decision","The Bot could not decide on the next step after several attempts. You can retry.","Retry decision"},
    {"t3_input","Action in T3","Open the session in T3 to approve or answer.",""},
    {"deadline","Deadline extension","This request reached its deadline. You can set a new one.","Extend deadline"},
    {"unknown_delivery","Delivery unconfirmed","Flock could not confirm whether T3 received the last message. Reconcile checks T3 again without resending anything.","Reconcile"},
    {"auth","T3 sign-in needed","Sign in to T3 again, then reconcile.","Reconcile"},
    {"settings","T3 settings","Fix the T3 connection or execution settings, then reconcile.","Reconcile"}
};

void describeWait(Json& view, std::string_view reason) {
    const auto copy = std::ranges::find(waitCopy,reason,&WaitCopy::reason);
    view["label"] = copy == std::end(waitCopy) ? "Waiting for input" : copy->label;
    if (field(view,"text").empty())
        view["text"] = copy == std::end(waitCopy) ? "Refresh the pending request." : copy->text;
    if (copy != std::end(waitCopy) && !copy->operationLabel.empty())
        view["operationLabel"] = copy->operationLabel;
}

// Adapter errors are useful in the CLI but can include connection settings. Publish the reason code.
Json waitView(const Json& wait) {
    if (wait.is_null()) return nullptr;
    Json view = Json::object();
    copyFields(view,wait,{"id","messageId","reason","decisionReason"});
    const auto reason = field(wait,"reason");
    if (reason == "user_question" || reason == "t3_input") copyFields(view,wait,{"text"});
    describeWait(view,reason);
    if (reason == "user_question" && wait.contains("choices") && wait["choices"].is_array()) {
        auto& choices = view["choices"] = Json::array();
        for (const auto& choice : wait["choices"])
            if (choice.is_string()) choices.push_back(choice);
    }
    return view;
}

std::string displayState(const Json& task);

Json sourceView(const Json& source, bool includeText = false) {
    Json view = Json::object();
    copyFields(view,source,{"taskId","messageId","revisionId","option"});
    if (includeText) copyFields(view,source,{"text"});
    return view;
}

Json eventView(const Json& event) {
    auto view = Json::object();
    const auto kind = field(event,"kind");
    copyFields(view,event,{"id","time","kind"});
    if (kind != "bot_message") copyFields(view,event,{"taskId"});
    const auto& data = event.at("data");
    Json details = Json::object();
    if (kind == "waiting") details = waitView(data);
    else if (kind == "request") copyFields(details,data,{"text","source","scheduleId","scheduledAt","messageId"});
    else if (kind == "wait_closed") copyFields(details,data,{"waitId","resolution"});
    else if (kind == "cancelled") copyFields(details,data,{"request"});
    else if (kind == "user_answer") copyFields(details,data,{"text","waitId","question"});
    else if (kind == "completed") {
        copyFields(details,data,{"text","reason","doneWhen","revisionId"});
        if (data.contains("source")) {
            const auto& source = data.at("source");
            auto run = runView(source,field(source,"environmentId"));
            if (!run.is_null()) {
                copyFields(run,source,{"itemId"});
                details["run"] = std::move(run);
            }
        }
    } else if (kind == "bot_message") {
        copyFields(details,data,{"exchangeId","messageId","fromBotId","toBotId","messageType","requestKind","text","replyTo"});
        if (const auto forwarded = data.find("forwardedMessage"); forwarded != data.end() && forwarded->is_object()) {
            const auto text = field(*forwarded,"text");
            const auto time = forwarded->find("time");
            if (!text.empty() && time != forwarded->end() && time->is_number_integer())
                details["forwardedMessage"] = {{"text",text},{"time",*time}};
        }
    } else if (kind == "report") {
        copyFields(details,data,{"report","status","resultRunId","truncated","itemId","revisionId"});
        details["run"] = runView(data.at("run"),field(data,"environmentId"));
    } else if (kind == "conversation_message" || kind == "conversation_reply") {
        copyFields(details,data,{"text","messageId","replyToMessageId","taskId","status","effectStatus","deliveredToT3"});
        if (const auto actions = data.find("actions"); actions != data.end() && actions->is_array()) {
            details["actions"] = Json::array();
            for (const auto& action : *actions) {
                Json item = Json::object();
                copyFields(item,action,{"tool","taskId","effect"});
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

std::string preview(Store& store, std::string_view bot) {
    const auto events = store.conversation(bot,0,12);
    std::string request;
    for (auto event = events.rbegin(); event != events.rend(); ++event) {
        const auto kind = field(*event,"kind");
        const auto& data = event->at("data");
        auto text = kind == "report" ? field(data,"report") : field(data,"text");
        if (kind == "bot_message" && text.empty() && data.contains("forwardedMessage"))
            text = field(data.at("forwardedMessage"),"text");
        if ((kind == "report" || kind == "completed" || kind == "conversation_reply" ||
             (kind == "waiting" && field(data,"reason") == "user_question")) && !text.empty())
            return bounded(text,240);
        if (request.empty() && (kind == "request" || kind == "conversation_message" || kind == "bot_message")) request = bounded(text,240);
    }
    return request;
}

std::string displayState(const Json& task) {
    const auto state = field(task,"state");
    if (state == "cancelling" || ((state != "cancelled" && state != "completed") && task.value("replanning",false)))
        return "stopping";
    return state;
}

Json taskSummary(const Json& task) {
    Json result = {{"state",displayState(task)}};
    copyFields(result,task,{"id","request","deadline","currentRevision","sourceTaskId","createdAt"});
    return result;
}

Json currentWait(const Json& task) {
    auto result = taskSummary(task);
    const auto& wait = task.at("wait");
    const auto env = field(task,"environmentId");
    auto& view = result["wait"] = waitView(wait);
    view["allowedOperations"] = waitOperations(task);
    view["run"] = runView(wait.at("run"),env);
    if (view["run"].is_null()) view["run"] = runView(task.at("lastRun"),env);
    if (!view["run"].is_null()) view["run"]["actionRequired"] = field(wait,"reason") == "t3_input";
    return result;
}

Json taskSessions(const Json& task) {
    auto sessions = Json::array();
    const auto env = field(task,"environmentId");
    const auto& activeRun = task.at("activeRun");
    const auto& runs = task.at("runs");
    std::set<std::pair<std::string,std::string>> seen;
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        auto session = runView(*run,env);
        if (session.is_null()) continue;
        const auto thread = field(session,"threadId");
        if (!seen.emplace(field(session,"environmentId"),thread).second) continue;
        const bool active = field(*run,"runId") == field(activeRun,"runId") && thread == field(activeRun,"threadId") &&
            field(session,"environmentId") == env;
        session["state"] = active ? displayState(task) : "ended";
        session["active"] = active;
        sessions.push_back(std::move(session));
    }
    return sessions;
}

Json activity(Store& store, std::string_view bot) {
    constexpr std::string_view priority[] = {"idle","queued","peer_waiting","waiting","thinking","working","stopping"};
    std::size_t highest = 0;
    Json result = {{"waits",Json::array()},{"queued",Json::array()},{"activeSessions",Json::array()}};
    for (const auto& task : store.activeTasks(bot)) {
        const auto state = field(task,"state");
        const auto display = displayState(task);
        const auto presence = display == "stopping" ? "stopping"
            : state == "running" ? "working" : state == "deciding" || state == "sending" ? "thinking" : state;
        for (std::size_t i = highest + 1; i < std::size(priority); ++i)
            if (presence == priority[i]) highest = i;
        if (state == "waiting") result["waits"].push_back(currentWait(task));
        const auto& blocker = task.at("blockedBy");
        if (state == "queued" && !blocker.is_null()) {
            auto queued = taskSummary(task);
            queued["blockedBy"] = {{"hidden",true}};
            if (field(blocker,"botId") == bot) {
                queued["blockedBy"] = taskSummary(blocker);
                copyFields(queued["blockedBy"],blocker,{"botId"});
            }
            result["queued"].push_back(std::move(queued));
        }
        auto active = runView(task.at("activeRun"),field(task,"environmentId"));
        if (!active.is_null()) {
            active["taskId"] = task.at("id");
            active["request"] = task.at("request");
            active["state"] = display;
            copyFields(active,task,{"createdAt"});
            result["activeSessions"].push_back(std::move(active));
        }
    }
    auto& paused = result["pausedConversations"] = Json::array();
    for (const auto& turn : store.pausedConversations(bot)) {
        Json view = {{"allowedOperations",Json::array({"approve"})}};
        copyFields(view,turn,{"messageId","text","status"});
        describeWait(view,"model_budget");
        paused.push_back(std::move(view));
    }
    if (!paused.empty()) {
        const auto waiting = std::ranges::find(priority,"waiting") - std::begin(priority);
        highest = std::max(highest,static_cast<std::size_t>(waiting));
    }
    result["presence"] = priority[highest];
    return result;
}

Json historyPage(const Json& events) {
    const bool more = events.size() > conversationSize;
    Json result = {{"events",Json::array()},{"olderBefore",0}};
    auto& page = result["events"];
    for (auto event = events.begin() + (more ? 1 : 0); event != events.end(); ++event)
        page.push_back(eventView(*event));
    if (more) result["olderBefore"] = page.front().at("id");
    return result;
}
} // namespace

Json botSettings(const Json& bot) {
    return settingsView(bot);
}

Json botList(Store& store, const Json& bots) {
    auto result = Json::array();
    for (const auto& bot : bots) {
        const auto id = field(bot,"id");
        if (id.empty()) continue;
        auto row = botSummary(bot);
        row["preview"] = preview(store,id);
        row["presence"] = activity(store,id).at("presence");
        result.push_back(std::move(row));
    }
    return result;
}


Json conversation(Store& store, std::string_view bot, Id before) {
    return historyPage(store.conversation(bot,before,conversationSize + 1));
}

Json peerConversation(Store& store, std::string_view bot, std::string_view peer, Id before) {
    return historyPage(store.peerConversation(bot,peer,before,conversationSize + 1));
}

Json sessions(Store& store, std::string_view bot, Id before) {
    const auto tasks = store.sessionTasks(bot,before,sessionsSize + 1);
    Json result = {{"sessionTasks",Json::array()},{"sessionsBefore",0}};
    auto& page = result["sessionTasks"];
    for (const auto& task : tasks) {
        if (page.size() == sessionsSize) {
            result["sessionsBefore"] = page.back()["id"];
            break;
        }
        auto group = taskSummary(task);
        group["sessions"] = taskSessions(task);
        page.push_back(std::move(group));
    }
    return result;
}

Json taskDetail(const Json& task, const Json& latestResult) {
    auto result = taskSummary(task);
    copyFields(result,task,{"doneWhen","intent"});
    result["result"] = latestResult.is_null() ? Json(nullptr) : eventView(latestResult);
    auto& revisions = result["revisions"] = Json::array();
    if (task.contains("revisions") && task["revisions"].is_array()) {
        for (const auto& revision : task["revisions"]) {
            Json view = Json::object();
            copyFields(view,revision,{"id","number","intent","doneWhen","sourceMessageId"});
            revisions.push_back(std::move(view));
        }
    }
    auto& instructions = result["pendingInstructions"] = Json::array();
    if (task.contains("pendingInstructions") && task["pendingInstructions"].is_array()) {
        for (const auto& instruction : task["pendingInstructions"]) {
            Json view = Json::object();
            copyFields(view,instruction,{"messageId","text","revisionId","conflicting"});
            instructions.push_back(std::move(view));
        }
    }
    auto& sources = result["sourceResultRefs"] = Json::array();
    if (task.contains("sourceResultRefs") && task["sourceResultRefs"].is_array()) {
        for (const auto& source : task["sourceResultRefs"]) sources.push_back(sourceView(source,true));
    }
    auto& runs = result["runs"] = Json::array();
    for (const auto& run : task.at("runs")) {
        auto view = runView(run,field(task,"environmentId"));
        if (view.is_null()) continue;
        copyFields(view,run,{"revisionId"});
        runs.push_back(std::move(view));
    }
    return result;
}

Json botView(Store& store, const Json& bot) {
    const auto id = field(bot,"id");
    auto result = conversation(store,id,0);
    result.update(activity(store,id));
    result.update(sessions(store,id,0));
    result["bot"] = settingsView(bot);
    return result;
}
} // namespace flockv0::web
