#include "flock/board_inbox.hpp"
#include "flock/session_read.hpp"

#include <iostream>
#include <sqlite3.h>
#include <map>
#include <unistd.h>

using namespace flockv0;

namespace {
constexpr auto staging = "internal-staging-thread";
constexpr auto second = "internal-second-investigation";
constexpr auto env = "desktop";
constexpr auto otherEnv = "remote-desktop";
constexpr auto timestamp = "2026-11-21T12:34:56.123Z";
constexpr auto failureId = "internal-failed-event";
constexpr auto logId = "internal-test-log";

void check(bool pass, const char* why) {
    if (!pass) throw std::runtime_error(why);
}

struct Backend : T3 {
    std::map<std::string,bool> denied;
    std::vector<Command> writes;
    int stops = 0, reads = 0;
    std::function<void()> onRead;
    std::string secret;
    bool denyAttachment = false;

    bool supportsRunTargetedSteer() const override { return true; }
    std::string authGeneration(const std::string&,const Json&) override { return "connected"; }
    Delivery send(const Command& command) override {
        writes.push_back(command);
        const RunRef run{"foreign-thread","foreign-run",{}};
        if (command.tool == "steer" &&
            (command.target.threadId != run.threadId || command.target.runId != run.runId))
            return {Delivery::Kind::NotAccepted,{},"The assigned Run is unavailable",true};
        return {Delivery::Kind::Accepted,run,{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef&) override { ++stops; return InterruptOutcome::Requested; }

    bool allowed(const SessionTarget& target) {
        ++reads;
        if (onRead) onRead();
        return (target.environmentId == env || target.environmentId == otherEnv) && !denied[target.threadId] &&
            (target.threadId == staging || target.threadId == second || target.threadId == "foreign-thread");
    }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target,bool includeMessages,
                                                  SessionReadDeadline) override {
        if (!allowed(target)) return {};
        std::string title = target.threadId == second ? "Staging investigation 2" : "Staging investigation";
        if (!secret.empty()) title.append(235-title.size(),'x').append(secret);
        SessionMetadata metadata{title,"Release Pipeline","release","failed",timestamp,timestamp,{},false};
        if (includeMessages) {
            std::string preview = "The staging regression failed.";
            if (!secret.empty()) preview += " " + secret;
            metadata.messages.push_back({std::move(preview),false});
        }
        return metadata;
    }
    SessionEvent item(unsigned i) {
        SessionEvent result{};
        result.id = i == 2 ? failureId : "internal-event-"+std::to_string(i);
        result.kind = i == 2 ? "command_execution" : (i == 3 ? "error" : "assistant_message");
        result.time = timestamp;
        result.status = i == 2 ? "failed" : "completed";
        result.text = i == 2 ? "$ cd build\nctest --output-on-failure\nTest 7 failed" : "Investigation note";
        if (i == 0 && !secret.empty()) result.text = std::string(479,'x') + "🙂";
        if (i == 3) result.text = "timeout";
        if (i == 4) {
            result.kind = "command_execution";
            result.text = "$ cd build\ntrue";
        }
        if (i == 2) {
            result.attachments.push_back({logId,"test.log","text/plain",{},false});
            if (!secret.empty()) {
                result.title = secret;
                result.text.append(476-result.text.size(),'x').append(secret);
                result.attachments.front().name = secret;
            }
        }
        if (i == 3 && !secret.empty()) {
            result.text += " " + secret.substr(0,12);
            result.textTruncated = true;
        }
        return result;
    }
    std::optional<SessionEventPage> sessionEvents(const SessionTarget& target,
                                                  const SessionEventFilter& filter) override {
        if (!allowed(target)) return {};
        SessionEventPage page;
        unsigned i = static_cast<unsigned>(filter.afterPosition.value_or(-1)+1);
        for (; i < 5 && page.items.size() < filter.limit; ++i) {
            auto event = item(i);
            if ((filter.kind.empty() || event.kind == filter.kind) &&
                (!filter.matchText || filter.matchText(event)) &&
                session_read::inRange(event.time,filter.fromTime,filter.toTime))
                page.items.push_back(std::move(event));
        }
        if (i < 5) page.nextPosition = i-1;
        return page;
    }
    std::optional<SessionEvent> sessionEvent(const SessionTarget& target,std::string_view id) override {
        if (!allowed(target)) return {};
        for (unsigned i = 0; i < 5; ++i)
            if (auto event = item(i); event.id == id) return event;
        return {};
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget& target) override {
        if (!allowed(target)) return {};
        return SessionCounts{11,3};
    }
    std::optional<SessionAttachment> sessionAttachment(const SessionTarget& target,std::string_view id,
                                                       std::string_view attachment) override {
        if (!allowed(target) || id != failureId || attachment != logId || denyAttachment) return {};
        if (!secret.empty()) return SessionAttachment{logId,secret,"text/plain",
            std::string(8188,'x')+secret+"🙂",false};
        return SessionAttachment{logId,"test.log","text/plain","fixture failed at the staging health check\n"+
                                 std::string(9000,'x'),false};
    }
};

struct Agent : Model {
    Decision next;
    Json result;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        if (input.event.at("kind") == "intake")
            return {"delegate",{{"instruction","Investigate staging"},{"doneWhen","Report failure"}},"Investigate"};
        const auto& steps = input.event.at("turn");
        if (!steps.empty()) {
            result = steps.back().at("result");
            return {"reply",{{"text","Checked."}},"Respond with the evidence"};
        }
        check(std::ranges::find(input.tools,next.tool) != input.tools.end(),"tool is absent from the conversation");
        return next;
    }
};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Agent model;
    Backend t3;
    int sequence = 0;

    Fixture() {
        char pattern[] = "/tmp/flock-session-read-XXXXXX";
        const auto* path = mkdtemp(pattern);
        if (!path) throw std::runtime_error("cannot create test state");
        dir = path;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(dir);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(dir/"state.db");
        runtime = std::make_unique<Runtime>(*store,model,t3,[](auto) { return std::string{}; });
    }
    Intake intake(std::string bot) {
        return {std::move(bot),"Investigate staging",env,"release",Json::object()};
    }
    Json call(std::string tool, Json args, std::string bot = "alpha") {
        model.result = nullptr;
        model.next = {std::move(tool),std::move(args),"Inspect the requested investigation"};
        store->receive(localOwner,intake(std::move(bot)),std::to_string(++sequence),
                       "Where did yesterday’s staging investigation fail?");
        runtime->runConversations();
        check(!model.result.is_null(),"conversation did not dispatch tool");
        return model.result;
    }
    void share(std::string_view thread, GrantChange change = GrantChange::grant) {
        store->access().resourceGrant({"root",true},"alpha",Scope::sessionRead(env,thread),change);
    }
};

void acceptance() {
    Fixture f;
    const auto search = "flock_session_search";
    const auto events = "flock_session_events";
    const Json filters{{"query","staging"},{"project","Release Pipeline"},{"status","failed"},
                       {"fromTime","2026-11-21T00:00:00Z"},{"toTime","2026-11-22T00:00:00Z"},{"limit",1}};
    const Json empty{{"ok",true},{"sessions",Json::array()},{"nextCursor",nullptr},{"truncated",false}};
    check(f.call(search,filters) == empty && f.t3.reads == 0,"unshared Session contacted T3 or entered search");
    const auto missing = f.call(events,{{"sessionRef","random"},{"operation","counts"}});
    check(missing == f.call(events,{{"sessionRef",staging},{"operation","counts"},{"bot","beta"}}) &&
          missing.at("ok") == false && f.t3.reads == 0,"manual thread ID exposed the unshared Session");

    f.store->access().personGrant({"root",true},"alpha","alice",Role::admin,GrantChange::grant);
    try {
        f.store->access().resourceGrant({"alice"},"alpha",Scope::sessionRead(env,staging),GrantChange::grant);
        throw std::runtime_error("an administrator shared a scope she does not hold");
    } catch (const Conflict&) {}
    f.share(staging);
    f.share(second);
    f.share("internal-shared-but-denied");
    const auto found = f.call(search,filters);
    const auto searchCursor = found.at("nextCursor");
    check(found.at("sessions").size() == 1 && searchCursor.is_string(),"search pagination missing");
    const auto after = f.call(search,filters);
    check(after == found,"search references changed for the same filter");
    auto nextSearch = filters;
    nextSearch["cursor"] = searchCursor;
    const auto next = f.call(search,nextSearch);
    check(next.at("sessions").size() == 1 && next.at("nextCursor").is_null() &&
          next.at("sessions")[0].at("title") != found.at("sessions")[0].at("title"),
          "search pagination repeated a candidate");
    check(found.dump().find("internal-") == std::string::npos && next.dump().find("internal-") == std::string::npos,
          "search returned an internal thread identifier");
    const auto preview = f.call(search,{{"query","regression failed"}});
    check(preview.at("sessions").size() == 2 &&
          preview.at("sessions").at(0).at("matchExcerpt") == "The staging regression failed.",
          "message-only search lost an authorized local match");
    check(f.call(search,{{"query","2"}}).at("sessions").size() == 1 &&
          f.call(search,{{"query",std::string(201,'x')}}) == empty,
          "Flock text filtering inherited T3 project-search limits");
    check(f.call(search,{{"fromTime","2026-11-21T12:34:56.123000000Z"},
                         {"toTime","2026-11-21T12:34:56.123Z"}}).at("sessions").size() == 2 &&
          f.call(search,{{"toTime","2026-11-21T12:34:56.122999999Z"}}) == empty &&
          f.call(search,{{"fromTime","2026-02-29T00:00:00Z"}}).at("ok") == false &&
          f.call(search,{{"limit",21}}).at("ok") == false,
          "UTC range or limit validation is incorrect");

    const auto sessionRef = next.at("sessions").at(0).at("sessionRef");
    const auto otherRef = found.at("sessions").at(0).at("sessionRef");
    Json read{{"sessionRef",sessionRef}};
    const auto request = [&](std::string_view operation, Json args = Json::object(), std::string bot = "alpha") {
        args.update(read);
        args["operation"] = operation;
        return f.call(events,args,std::move(bot));
    };
    const auto first = request("list",{{"limit",2}});
    const auto cursor = first.at("nextCursor");
    const auto secondPage = request("list",{{"cursor",cursor},{"limit",2}});
    check(first.at("events").size() == 2 && cursor.is_string() && secondPage.at("events").size() == 2 &&
          secondPage.at("events")[0].at("kind") == "command_execution","ordered event pagination failed");
    const auto foundEvent = request("search",{{"kind","command_execution"},{"query","FAILED"},{"fromTime",timestamp}});
    const auto eventRef = foundEvent.at("events").at(0).at("eventRef");
    check(foundEvent.at("events").size() == 1 && eventRef == secondPage.at("events")[0].at("eventRef"),
          "filtered event search failed");
    const auto detail = request("read_event",{{"eventRef",eventRef}});
    const auto& event = detail.at("event");
    const auto attachmentRef = event.at("attachments").at(0).at("attachmentRef");
    check(event.at("text") == "$ cd build\nctest --output-on-failure\nTest 7 failed" &&
          !event.contains("command") && !event.contains("output") &&
          !detail.at("truncated").get<bool>() &&
          detail.dump().find("internal-") == std::string::npos,"failed event details or reference scope incorrect");
    const auto last = request("list",{{"limit",5}}).at("events");
    const auto quiet = request("read_event",{{"eventRef",last.at(4).at("eventRef")}}).at("event");
    check(quiet.at("text") == "$ cd build\ntrue" && !quiet.contains("output"),
          "multiline command lines were reported as output");
    check(request("counts",{{"limit",1},{"kind","error"}}).at("counts") == Json{{"messages",11},{"runs",3}},
          "counts were scoped to a page or filter");
    check(request("counts",{{"bot","alpha"}},"beta") == missing &&
          f.call(events,{{"sessionRef",otherRef},{"operation","read_event"},{"eventRef",eventRef}}) == missing &&
          f.call(events,{{"sessionRef",otherRef},{"operation","get_attachment"},{"attachmentRef",attachmentRef}}) == missing,
          "references crossed a Bot or Session boundary");
    check(request("list",{{"cursor",cursor},{"kind","error"}}) == missing,"cursor reused with different filters");
    const auto missingAttachment = request("get_attachment",{{"attachmentRef","missing"}});
    const auto fetchLog = [&] { return request("get_attachment",{{"attachmentRef",attachmentRef}}); };
    const auto log = fetchLog();
    check(log.at("attachment").at("content").get<std::string>().starts_with("fixture failed") &&
          log.at("attachment").at("content").get<std::string>().size() == 8192 && log.at("truncated") == true,
          "attachment content or truncation was lost");
    f.store->access().resourceGrant({"root",true},"alpha",Scope::sessionRead(otherEnv,staging),GrantChange::grant);
    const auto acrossEnvironments = f.call(search,Json::object()).at("sessions");
    const auto remoteRef = acrossEnvironments.back().at("sessionRef");
    check(acrossEnvironments.size() == 3 && remoteRef != sessionRef &&
          f.call(events,{{"sessionRef",remoteRef},{"operation","read_event"},{"eventRef",eventRef}}).dump() == missing.dump() &&
          f.call(events,{{"sessionRef",remoteRef},{"operation","list"},{"cursor",cursor}}).dump() == missing.dump() &&
          f.call(events,{{"sessionRef",remoteRef},{"operation","get_attachment"},{"attachmentRef",attachmentRef}}).dump() ==
              missingAttachment.dump(),
          "references crossed environments for the same T3 thread identifier");
    f.store->access().resourceGrant({"root",true},"alpha",Scope::sessionRead(otherEnv,staging),GrantChange::revoke);
    auto tampered = sessionRef.get<std::string>();
    tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
    check(f.call(events,{{"sessionRef",tampered},{"operation","counts"}}).dump() == missing.dump() &&
          f.call(events,{{"operation","counts"}}).dump() == missing.dump() &&
          request("list",{{"cursor",std::string(129,'x')}}).dump() == missing.dump() &&
          request("get_attachment",{{"attachmentRef",std::string(129,'x')}}).dump() == missingAttachment.dump(),
          "invalid or missing references produced a distinct response");
    f.restart();
    check(fetchLog() == log && request("list",{{"cursor",cursor},{"limit",2}}) == secondPage &&
          f.call(search,nextSearch) == next,"restart lost scoped references or the read grant");

    f.store->access().resourceGrant({"root",true},"beta",{"environment",env,"launch"},GrantChange::grant);
    const auto task = f.store->submit(f.intake("beta"));
    for (int i = 0; i < 8 && f.store->task(task).at("state") != "running"; ++i) f.runtime->tick();
    const auto running = f.store->task(task);
    check(running.at("state") == "running" && f.t3.writes.size() == 1,"foreign task did not start");
    const auto foreign = Scope::sessionRead(env,"foreign-thread");
    check(f.store->access().permits("beta",foreign,true) && !f.store->access().permits("alpha",foreign,true),
          "owned Session access is incorrect");
    check(f.call(search,Json::object(),"beta").at("sessions").size() == 1,"owned Session missing without a grant");
    f.t3.denied["foreign-thread"] = true;
    check(f.call(search,Json::object(),"beta") == empty,"T3 denial did not override Session ownership");
    f.t3.denied["foreign-thread"] = false;
    f.share("foreign-thread");
    for (auto tool : {"message_session","interrupt_session","send_to_task","cancel_task"}) {
        const auto outcome = f.call(tool,{{"taskId",task},{"generation",running.at("generation")},
                                         {"text","Deliver this"},{"instruction","Deliver this"}});
        check(outcome.at("ok") == false,"read grant enabled a write or interruption");
    }
    check(f.t3.writes.size() == 1 && !f.t3.stops,"write tool bypassed task ownership");

    f.t3.denied[staging] = true;
    check(request("list",{{"cursor",cursor},{"limit",2}}) == missing &&
          request("counts") == missing && fetchLog() == missingAttachment &&
          f.call(search,nextSearch) == empty,"T3 denial did not override the active grant");
    check(request("list",{{"fromTime","invalid"}}).dump() ==
          f.call(events,{{"operation","list"},{"sessionRef","missing"},{"fromTime","invalid"}}).dump() &&
          request("unknown").dump() == f.call(events,{{"operation","unknown"},{"sessionRef","missing"}}).dump(),
          "request validation depended on whether a Session existed");
    f.t3.denied[staging] = false;
    f.share(second,GrantChange::revoke);
    auto missingSearch = nextSearch;
    missingSearch["cursor"] = "missing";
    check(f.call(search,nextSearch).dump() == f.call(search,missingSearch).dump() &&
          f.call(search,nextSearch) == missing,"revoked search cursor still returned a page");

    f.t3.onRead = [&] {
        Store writer(f.dir/"state.db");
        writer.access().resourceGrant({"root",true},"alpha",Scope::sessionRead(env,staging),GrantChange::revoke);
    };
    check(request("read_event",{{"eventRef",eventRef}}) == missing,"revocation during an upstream read was ignored");
    f.t3.onRead = {};
    check(request("list",{{"cursor",cursor},{"limit",2}}) == missing &&
          request("read_event",{{"eventRef",eventRef}}) == missing && fetchLog() == missingAttachment &&
          request("counts") == missing,"revoked Session references still exposed evidence");
    f.share("foreign-thread",GrantChange::revoke);
    check(f.call(search,filters) == empty,"revoked or unauthorized Sessions returned to search");
}

void approvedCallSurvivesReadPreflightContention() {
    Fixture f;
    f.share(staging);
    f.model.next = {"flock_session_search",{{"query","regression"}},"Find the report"};
    auto intake = f.intake("alpha");
    intake.config["modelCallLimit"] = 1;
    const auto receipt = f.store->receive(localOwner,intake,"preflight-busy","Look for the failure");
    f.runtime->runConversations();
    const auto paused = f.store->pausedConversations("alpha");
    check(paused.size() == 1 && f.model.result.is_null(),"turn did not pause after reading evidence");
    const Id pauseId = paused.front().at("messageId");
    f.store->decideInbox(localOwner,{"alpha",InboxKind::conversation_model_budget,receipt.at("messageId"),pauseId,0,0,pauseId},
                         "approve");
    sqlite3* raw = nullptr;
    check(sqlite3_open(f.store->path().c_str(),&raw) == SQLITE_OK,"could not open the second writer");
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> writer(raw,sqlite3_close);
    bool locked = false;
    f.t3.onRead = [&] {
        if (!std::exchange(locked,true))
            check(sqlite3_exec(writer.get(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr) == SQLITE_OK,"lock writer");
    };
    bool contended = false;
    try { f.runtime->runConversations(); }
    catch (const std::runtime_error& error) { contended = std::string_view(error.what()) == "database is locked"; }
    check(locked && contended && f.model.result.is_null(),"pre-provider write contention was not exercised");
    check(sqlite3_exec(writer.get(),"ROLLBACK",nullptr,nullptr,nullptr) == SQLITE_OK,"could not release writer");
    f.t3.onRead = {};
    f.runtime->runConversations();
    const auto after = f.store->pausedConversations("alpha");
    check(!f.model.result.is_null() && after.empty(),"Conversation lost its approved call before model dispatch");
}

void redactionAndResumedHistory() {
    Fixture f;
    f.share(staging);
    auto access = f.store->access();
    const auto secret = "synthetic-session-credential";
    const auto key = access.requestSecret("beta",{"beta-fixture"});
    access.submitSecret({"root",true},"beta",key,secret);
    f.t3.secret = secret;

    const auto found = f.call("flock_session_search",{{"query","regression"}});
    const auto& row = found.at("sessions").front();
    check(found.dump().find("synt") == std::string::npos &&
          row.at("matchExcerpt") == "The staging regression failed. [redacted]" &&
          found.at("truncated") == true,"search exposed a protected value across an excerpt boundary");
    const auto ref = row.at("sessionRef");
    const auto events = f.call("flock_session_events",{{"sessionRef",ref},{"operation","list"}});
    const auto& failed = events.at("events").at(2);
    const auto& error = events.at("events").at(3);
    check(events.dump().find("synt") == std::string::npos && failed.at("title") == "[redacted]" &&
          events.at("events").front().at("excerpt").get<std::string>().size() == 479 &&
          error.at("excerpt") == "timeout [redacted]" && error.at("truncated") == true,
          "list exposed event metadata or a truncated protected value");
    const auto detail = f.call("flock_session_events",{{"sessionRef",ref},{"operation","read_event"},
                                                       {"eventRef",error.at("eventRef")}});
    check(detail.at("event").at("error") == "timeout [redacted]","event error was not redacted");
    const Json attachment{{"sessionRef",ref},{"operation","get_attachment"},
                          {"attachmentRef",failed.at("attachments").front().at("attachmentRef")}};
    const auto log = f.call("flock_session_events",attachment);
    check(log.at("attachment").at("content").get<std::string>().size() == 8192 &&
          log.at("attachment").at("name") == "[redacted]" && log.dump().find("synt") == std::string::npos &&
          log.at("truncated") == true,"attachment exposed a protected value at the byte limit");

    const auto missing = f.call("flock_session_events",{{"sessionRef","unknown"},{"operation","counts"}});
    const auto pauseThenRevoke = [&](Decision step, const std::function<void()>& revoke) {
        f.model.next = std::move(step);
        f.model.result = nullptr;
        auto intake = f.intake("alpha");
        intake.config["modelCallLimit"] = 1;
        const auto receipt = f.store->receive(localOwner,intake,std::to_string(++f.sequence),"Look for the failure");
        f.runtime->runConversations();
        const auto paused = f.store->pausedConversations("alpha");
        check(paused.size() == 1 && f.model.result.is_null(),"turn did not pause after reading evidence");
        revoke();
        f.restart();
        const Id pauseId = paused.front().at("messageId");
        f.store->decideInbox(localOwner,{"alpha",InboxKind::conversation_model_budget,receipt.at("messageId"),pauseId,0,0,pauseId},
                             "approve");
        f.runtime->runConversations();
        check(f.model.result.dump() == missing.dump(),"reopened turn exposed inaccessible cached evidence");
    };
    pauseThenRevoke({"flock_session_events",attachment,"Read the log"},[&] { f.t3.denyAttachment = true; });
    pauseThenRevoke({"flock_session_search",Json::object(),"Find staging"},[&] { f.t3.denied[staging] = true; });
    f.t3.denied[staging] = false;
    pauseThenRevoke({"flock_session_search",Json::object(),"Find staging"},[&] { f.share(staging,GrantChange::revoke); });
}
}

int main() {
    try {
        acceptance();
        redactionAndResumedHistory();
        approvedCallSurvivesReadPreflightContention();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
