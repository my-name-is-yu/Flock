#include "flock/access.hpp"
#include "flock/session_read.hpp"

#include <iostream>
#include <map>
#include <unistd.h>

using namespace flockv0;

namespace {
constexpr auto staging = "internal-staging-thread";
constexpr auto second = "internal-second-investigation";
constexpr auto env = "desktop";
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

    std::string authGeneration(const std::string&,const Json&) override { return "connected"; }
    Delivery send(const Command& command) override {
        writes.push_back(command);
        return {Delivery::Kind::Accepted,{"foreign-thread","foreign-run",{}},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
    void interrupt(const std::string&,const Json&,const RunRef&) override { ++stops; }

    bool allowed(const SessionTarget& target) {
        ++reads;
        if (onRead) onRead();
        return target.environmentId == env && !denied[target.threadId] &&
            (target.threadId == staging || target.threadId == second || target.threadId == "foreign-thread");
    }
    std::optional<SessionMetadata> findSession(const SessionTarget& target,std::string_view query) override {
        if (!allowed(target) || (!query.empty() && query != "staging")) return {};
        const std::string title = target.threadId == second ? "Second staging investigation" : "Staging investigation";
        return SessionMetadata{title,title,"Release Pipeline","release","failed",timestamp,timestamp,
                               "The staging regression failed."};
    }
    SessionEvent item(unsigned i) {
        SessionEvent result{};
        result.id = i == 2 ? failureId : "internal-event-"+std::to_string(i);
        result.kind = i == 2 ? "command_execution" : "assistant_message";
        result.time = timestamp;
        result.status = i == 2 ? "failed" : "completed";
        result.text = i == 2 ? "integration test failed" : "Investigation note";
        if (i == 2) {
            result.command = "ctest --output-on-failure";
            result.output = "Test 7 failed";
            result.error = "timeout";
            result.attachments.push_back({logId,"test.log","text/plain",{},false});
        }
        return result;
    }
    std::optional<SessionEventPage> sessionEvents(const SessionTarget& target,
                                                  const SessionEventFilter& filter) override {
        if (!allowed(target)) return {};
        SessionEventPage page;
        unsigned i = filter.cursor.empty() ? 0U : static_cast<unsigned>(std::stoul(filter.cursor));
        for (; i < 5 && page.items.size() < filter.limit; ++i) {
            auto event = item(i);
            if ((filter.kind.empty() || event.kind == filter.kind) &&
                session_read::contains(event.text,filter.query) &&
                session_read::inRange(event.time,filter.fromTime,filter.toTime))
                page.items.push_back(std::move(event));
        }
        if (i < 5) page.nextCursor = std::to_string(i);
        return page;
    }
    std::optional<SessionEvent> sessionEvent(const SessionTarget& target,std::string_view id) override {
        if (!allowed(target) || id != failureId) return {};
        return item(2);
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget& target) override {
        if (!allowed(target)) return {};
        return SessionCounts{11,3};
    }
    std::optional<SessionAttachment> sessionAttachment(const SessionTarget& target,std::string_view id,
                                                       std::string_view attachment) override {
        if (!allowed(target) || id != failureId || attachment != logId) return {};
        return SessionAttachment{logId,"test.log","text/plain","fixture failed at the staging health check\n"+
                                 std::string(9000,'x'),false};
    }
};

struct Agent : Model {
    Decision next;
    Json result;
    Decision decide(const ModelInput& input) override {
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
        runtime = std::make_unique<Runtime>(*store,model,t3,[](auto) { return BotNotes{}; });
    }
    Intake intake(std::string bot) {
        return {std::move(bot),"Investigate staging",env,"release",Json::object(),{},0};
    }
    Json call(std::string tool, Json args, std::string bot = "alpha") {
        model.result = nullptr;
        model.next = {std::move(tool),std::move(args),"Inspect the requested investigation"};
        store->receive(intake(std::move(bot)),std::to_string(++sequence),
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
    const Json empty{{"ok",true},{"sessions",Json::array()},{"nextCursor",nullptr}};
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
    check(event.at("command") == "ctest --output-on-failure" && event.at("output") == "Test 7 failed" &&
          event.at("error") == "timeout" && !detail.at("truncated").get<bool>() &&
          detail.dump().find("internal-") == std::string::npos,"failed event details or reference scope incorrect");
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
    f.t3.denied[staging] = false;
    f.share(second,GrantChange::revoke);
    check(f.call(search,nextSearch) == missing,"revoked search cursor still returned a page");

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
}

int main() {
    try {
        acceptance();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
