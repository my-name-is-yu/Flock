#include "views.hpp"

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
    Json view = Json::object();
    copyFields(view,source,{"kind","example","icon","color","eyeColor","filename","version"});
    if (field(source,"kind") == "upload") {
        const auto version = field(source,"version");
        const bool hasImage = !version.empty() || !field(source,"file").empty() || !field(source,"imageUrl").empty();
        if (hasImage && !botId.empty()) {
            auto url = "/api/bot-avatar?bot=" + threadSegment(botId);
            if (!version.empty()) url += "&v=" + threadSegment(version);
            view["imageUrl"] = std::move(url);
        }
    }
    const auto definition = source.value("definition",source);
    Json shape = Json::object();
    copyFields(shape,definition,{"version","id","name"});
    if (definition.contains("body")) copyFields(shape["body"] = Json::object(),definition["body"],{"path"});
    if (definition.contains("eyes")) copyFields(shape["eyes"] = Json::object(),definition["eyes"],{"x","y","spacing","scale"});
    if (shape.contains("body") && shape.contains("eyes")) view["definition"] = shape;
    return view;
}

Json botSummary(const Json& bot) {
    const auto id = field(bot,"id");
    const auto name = field(bot,"name");
    Json summary = {{"id",id},{"name",name.empty() ? id : name}};
    copyFields(summary,bot,{"role"});
    if (const auto avatar = bot.find("avatar"); avatar != bot.end() && avatar->is_object())
        summary["avatar"] = avatarView(*avatar,id);
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

// Adapter errors are useful in the CLI but can include connection settings. Publish the reason code.
Json waitView(const Json& wait) {
    if (wait.is_null()) return nullptr;
    Json view = Json::object();
    copyFields(view,wait,{"id","messageId","reason","decisionReason"});
    const auto reason = field(wait,"reason");
    if (reason == "user_question" || reason == "t3_input") copyFields(view,wait,{"text"});
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
        copyFields(details,data,{"text","messageId","replyToMessageId","taskId","route","status","deliveredToT3"});
        if (const auto channel = data.find("channel"); channel != data.end() && channel->is_object())
            copyFields(details["channel"] = Json::object(),*channel,{"channelKind","workspaceId","threadRef","externalMessageId"});
        for (const auto* key : {"attachments","artifacts"}) {
            const auto items = data.find(key);
            if (items != data.end() && items->is_array()) details[key] = *items;
        }
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
    view["run"] = runView(wait.at("run"),env);
    if (view["run"].is_null()) view["run"] = runView(task.at("lastRun"),env);
    return result;
}

Json sessionGroup(const Json& task) {
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
        sessions.push_back(std::move(session));
    }
    auto group = taskSummary(task);
    group["sessions"] = std::move(sessions);
    group["runCount"] = runs.size();
    return group;
}

std::string_view botPresence(const Json& activeTasks) {
    constexpr std::string_view priority[] = {"idle","queued","peer_waiting","waiting","thinking","working","stopping"};
    std::size_t highest = 0;
    for (const auto& task : activeTasks) {
        auto presence = displayState(task);
        if (presence == "running") presence = "working";
        else if (presence == "deciding" || presence == "sending") presence = "thinking";
        for (std::size_t i = highest + 1; i < std::size(priority); ++i)
            if (presence == priority[i]) highest = i;
    }
    return priority[highest];
}

Json activity(const Json& activeTasks, std::string_view bot) {
    Json result = {{"presence",botPresence(activeTasks)},{"waits",Json::array()},
                   {"queued",Json::array()},{"activeSessions",Json::array()}};
    for (const auto& task : activeTasks) {
        const auto state = field(task,"state");
        if (state == "waiting") result["waits"].push_back(currentWait(task));
        const auto& blocker = task.at("blockedBy");
        if (state == "queued" && !blocker.is_null()) {
            auto queued = taskSummary(task);
            queued["blockedBy"] = Json::object();
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
            active["state"] = displayState(task);
            copyFields(active,task,{"createdAt"});
            result["activeSessions"].push_back(std::move(active));
        }
    }
    return result;
}
} // namespace

Json botSettings(const Json& bot) {
    return settingsView(bot);
}

Json botList(Store& store, const Json& bots) {
    auto result = Json::array();
    if (bots.empty()) return result;
    const auto statesByBot = store.activeTaskStatesByBot();
    for (const auto& bot : bots) {
        const auto id = field(bot,"id");
        if (id.empty()) continue;
        auto row = botSummary(bot);
        row["preview"] = preview(store,id);
        row["presence"] = botPresence(statesByBot.value(id,Json::array()));
        result.push_back(std::move(row));
    }
    return result;
}

Json conversation(Store& store, std::string_view bot, Id before) {
    const auto events = store.conversation(bot,before,conversationSize + 1);
    const bool more = events.size() > conversationSize;
    Json result = {{"events",Json::array()},{"olderBefore",0}};
    auto& page = result["events"];
    for (auto event = events.begin() + (more ? 1 : 0); event != events.end(); ++event)
        page.push_back(eventView(*event));
    if (more) result["olderBefore"] = page.front().at("id");
    return result;
}

Json peerConversation(Store& store, std::string_view bot, std::string_view peer, Id before) {
    const auto events = store.peerConversation(bot,peer,before,conversationSize + 1);
    const bool more = events.size() > conversationSize;
    Json result = {{"messages",Json::array()},{"hasOlder",more}};
    auto& page = result["messages"];
    for (auto event = events.begin() + (more ? 1 : 0); event != events.end(); ++event)
        page.push_back(eventView(*event));
    return result;
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
        page.push_back(sessionGroup(task));
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

Json botView(Store& store, const Json& bot, const Json* changedSessionTasks) {
    const auto id = field(bot,"id");
    auto result = conversation(store,id,0);
    const auto activeTasks = store.activeTasks(id);
    result.update(activity(activeTasks,id));
    if (changedSessionTasks) {
        auto& groups = result["sessionTasks"] = Json::array();
        for (const auto& task : *changedSessionTasks) groups.push_back(sessionGroup(task));
    } else {
        result.update(sessions(store,id,0));
    }
    result["bot"] = settingsView(bot);
    auto& paused = result["pausedConversations"] = Json::array();
    for (const auto& turn : store.pausedConversations(id)) {
        Json view = Json::object();
        copyFields(view,turn,{"messageId","text","route"});
        paused.push_back(std::move(view));
    }
    if (!paused.empty() && (result["presence"] == "idle" || result["presence"] == "queued"))
        result["presence"] = "waiting";
    return result;
}
} // namespace flockv0::web
