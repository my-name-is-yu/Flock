#include "views.hpp"

#include <array>
#include <charconv>
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
constexpr unsigned taskBatch = 100;

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
    const auto thread = field(run,"threadId");
    if (environment.empty() || thread.empty() || environment == "." || environment == ".." || thread == "." || thread == "..")
        return nullptr;
    const auto path = "/" + threadSegment(environment) + "/" + threadSegment(thread);
    const auto saved = field(run,"link");
    return {{"threadId",thread},{"runId",field(run,"runId")},
            {"link",localThreadLink(saved,path) ? saved : "https://app.t3.codes/threads" + path}};
}

Json avatarView(const Json& source) {
    if (source.is_string()) return source;
    Json view = Json::object();
    copyFields(view,source,{"example","color","eyeColor"});
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
    if (bot.contains("avatar") && (bot["avatar"].is_string() || bot["avatar"].is_object()))
        summary["avatar"] = avatarView(bot["avatar"]);
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
            copyFields(row,schedule,{"id","schedule","expression","cron","timezone","timeZone","request","enabled"});
            cron.push_back(std::move(row));
        }
    }
    return view;
}

// Adapter errors are useful in the CLI but can include connection settings. Publish the reason code.
Json waitView(const Json& wait) {
    if (wait.is_null()) return nullptr;
    Json view = Json::object();
    copyFields(view,wait,{"id","reason","decisionReason"});
    const auto reason = field(wait,"reason");
    if (reason == "user_question" || reason == "t3_input") copyFields(view,wait,{"text"});
    if (reason == "user_question" && wait.contains("choices") && wait["choices"].is_array()) {
        auto& choices = view["choices"] = Json::array();
        for (const auto& choice : wait["choices"])
            if (choice.is_string()) choices.push_back(choice);
    }
    return view;
}

Json eventView(const Json& event) {
    auto view = Json::object();
    copyFields(view,event,{"id","taskId","time","kind"});
    const auto kind = field(event,"kind");
    const auto& data = event.at("data");
    Json details = Json::object();
    if (kind == "waiting") details = waitView(data);
    else if (kind == "request") copyFields(details,data,{"text","source","scheduleId","scheduledAt"});
    else if (kind == "wait_closed") copyFields(details,data,{"waitId","resolution"});
    else if (kind == "user_answer") copyFields(details,data,{"text","waitId"});
    else if (kind == "completed") copyFields(details,data,{"text","reason","doneWhen"});
    else if (kind == "bot_message") {
        copyFields(details,data,{"exchangeId","messageId","fromBotId","toBotId","messageType","requestKind","text","replyTo"});
        const auto evidence = data.find("evidence");
        if (evidence != data.end() && evidence->is_array()) {
            auto& references = details["evidence"] = Json::array();
            for (const auto& reference : *evidence) {
                auto run = runView(reference,field(reference,"environmentId"));
                if (run.is_null()) continue;
                copyFields(run,reference,{"itemId"});
                references.push_back(std::move(run));
            }
        }
    }
    else if (kind == "report") {
        copyFields(details,data,{"report","status","resultRunId","truncated","itemId"});
        details["run"] = runView(data.at("run"),field(data,"environmentId"));
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
        const auto text = kind == "report" ? field(data,"report") : field(data,"text");
        if ((kind == "report" || kind == "completed" ||
             (kind == "waiting" && field(data,"reason") == "user_question")) && !text.empty())
            return bounded(text,240);
        if (request.empty() && (kind == "request" || kind == "bot_message")) request = bounded(text,240);
    }
    return request;
}

Json taskSummary(const Json& task) {
    Json result = Json::object();
    copyFields(result,task,{"id","request","state","deadline"});
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

Json taskSessions(const Json& task) {
    auto sessions = Json::array();
    const auto env = field(task,"environmentId");
    const auto& activeRun = task.at("activeRun");
    const auto& runs = task.at("runs");
    std::set<std::string,std::less<>> seen;
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        const auto thread = field(*run,"threadId");
        if (!seen.insert(thread).second) continue;
        auto session = runView(*run,env);
        if (session.is_null()) continue;
        const bool active = field(*run,"runId") == field(activeRun,"runId") && thread == field(activeRun,"threadId");
        session["state"] = active ? field(task,"state") : "ended";
        session["environmentId"] = env;
        sessions.push_back(std::move(session));
    }
    return sessions;
}

// Store exposes a task cursor rather than separate queries for waits or Sessions.
// Scan bounded batches to keep older, still-open waits visible behind newer requests.
Json activity(Store& store, std::string_view bot, Id cursor, bool overview) {
    Json result = {{"sessionTasks",Json::array()},{"sessionsBefore",0}};
    if (overview) {
        result["waits"] = Json::array();
        result["queued"] = Json::array();
        result["activeSessions"] = Json::array();
        result["presence"] = "idle";
    }
    auto& sessionTasks = result["sessionTasks"];
    bool moreSessions = false;
    for (;;) {
        const auto tasks = store.tasks(bot,cursor,taskBatch);
        for (const auto& task : tasks) {
            const auto id = task.at("id").get<Id>();
            const auto state = field(task,"state");
            if (overview) {
                auto& presence = result["presence"];
                if (state == "waiting") {
                    result["waits"].push_back(currentWait(task));
                    if (presence != "working" && presence != "thinking") presence = "waiting";
                } else if (state == "running" || state == "cancelling") presence = "working";
                else if ((state == "deciding" || state == "sending") && presence != "working") presence = "thinking";
                else if (state == "peer_waiting" && presence == "idle") presence = "peer_waiting";
                if (state == "queued" && !task.at("blockedBy").is_null()) {
                    auto queued = taskSummary(task);
                    queued["blockedBy"] = taskSummary(task.at("blockedBy"));
                    copyFields(queued["blockedBy"],task.at("blockedBy"),{"botId"});
                    result["queued"].push_back(std::move(queued));
                }
                const auto& active = task.at("activeRun");
                if (!active.is_null()) {
                    result["activeSessions"].push_back({{"taskId",id},{"threadId",field(active,"threadId")},
                        {"runId",field(active,"runId")},{"state",state}});
                }
            }
            if (task.at("runs").empty()) continue;
            if (sessionTasks.size() < sessionsSize) {
                auto group = taskSummary(task);
                group["sessions"] = taskSessions(task);
                sessionTasks.push_back(std::move(group));
                result["sessionsBefore"] = id;
            } else {
                moreSessions = true;
                if (!overview) return result;
            }
        }
        if (tasks.size() < taskBatch) break;
        cursor = tasks.back().at("id").get<Id>();
    }
    if (!moreSessions) result["sessionsBefore"] = 0;
    return result;
}
} // namespace

Json botList(Store& store, const Json& bots) {
    auto result = Json::array();
    for (const auto& bot : bots) {
        const auto id = field(bot,"id");
        if (id.empty()) continue;
        auto row = botSummary(bot);
        row["preview"] = preview(store,id);
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

Json sessions(Store& store, std::string_view bot, Id before) {
    return activity(store,bot,before,false);
}

Json botView(Store& store, const Json& bot) {
    const auto id = field(bot,"id");
    auto result = conversation(store,id,0);
    result.update(activity(store,id,0,true));
    result["bot"] = settingsView(bot);
    return result;
}
} // namespace flockv0::web
