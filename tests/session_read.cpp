#include "flock/tasks.hpp"
#include "flock/secret_ref.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/session_read.hpp"
#include "flock/t3_connection.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/session_ref.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <sqlite3.h>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace flockv0;

namespace {
constexpr auto staging = "internal-staging-thread";
constexpr auto second = "internal-second-investigation";
constexpr auto env = "desktop";
constexpr auto otherEnv = "remote-desktop";
constexpr auto nativeSession = "internal-native-session";
constexpr auto timestamp = "2026-11-21T12:34:56.123Z";
constexpr auto failureId = "internal-failed-event";
constexpr auto logId = "internal-test-log";

void check(bool pass, const char* why) {
    if (!pass) throw std::runtime_error(why);
}

struct Backend : T3 {
    std::map<std::string, bool> denied;
    std::vector<Command> writes;
    int stops = 0, reads = 0;
    std::function<void()> onRead;
    std::string secret;
    bool denyAttachment = false;
    bool pagedCatalog = false;
    bool overlappingCatalog = false;
    bool nativeCatalog = false;
    std::vector<Id> catalogOffsets;

    std::string authGeneration(const std::string&, const Json&) override { return "connected"; }
    Delivery send(const Command& command) override {
        writes.push_back(command);
        return {Delivery::Kind::Accepted, {"foreign-thread", "foreign-run", {}}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override {
        ++stops;
        return InterruptOutcome::Requested;
    }

    bool allowed(const SessionTarget& target) {
        ++reads;
        if (onRead) onRead();
        if (target.threadId == nativeSession) return nativeCatalog && target.environmentId == otherEnv && !denied[target.threadId];
        return (target.environmentId == env || target.environmentId == otherEnv) && !denied[target.threadId] &&
               (target.threadId == staging || target.threadId == second || target.threadId == "foreign-thread");
    }
    std::pair<std::vector<std::string>, std::optional<Id>> projectSessions(std::string_view environment, std::string_view project,
                                                                           Id offset, SessionReadDeadline) override {
        if (overlappingCatalog) return {environment == env ? std::vector<std::string>{staging} : std::vector<std::string>{}, std::nullopt};
        if (pagedCatalog) {
            if (environment == otherEnv) return {};
            catalogOffsets.push_back(offset);
            if (offset == 0) return {{staging}, 1000};
            if (offset == 1000) return {{second}, std::nullopt};
            return {};
        }
        if (environment == otherEnv) {
            if (nativeCatalog && project.empty()) return {{nativeSession, staging}, std::nullopt};
            return {{staging}, std::nullopt};
        }
        return {{staging, second, "internal-shared-but-denied"}, std::nullopt};
    }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool includeMessages, SessionReadDeadline) override {
        if (!allowed(target)) return {};
        if (target.threadId == nativeSession)
            return SessionMetadata{"Native investigation", "Other Project", "other-project", "completed", timestamp, timestamp, {}, false};
        std::string title = target.threadId == second             ? "Staging investigation 2"
                            : target.threadId == "foreign-thread" ? "Foreign investigation"
                                                                  : "Staging investigation";
        if (!secret.empty()) title.append(235 - title.size(), 'x').append(secret);
        SessionMetadata metadata{title, "Release Pipeline", "release", "failed", timestamp, timestamp, {}, false};
        if (includeMessages) {
            std::string preview = "The staging regression failed.";
            if (!secret.empty()) preview += " " + secret;
            metadata.messages.push_back({std::move(preview), false});
        }
        return metadata;
    }
    SessionEvent item(unsigned i) {
        SessionEvent result{};
        result.id = i == 2 ? failureId : "internal-event-" + std::to_string(i);
        result.kind = i == 2 ? "command_execution" : (i == 3 ? "error" : "assistant_message");
        result.time = timestamp;
        result.status = i == 2 ? "failed" : "completed";
        result.text = i == 2 ? "$ cd build\nctest --output-on-failure\nTest 7 failed" : "Investigation note";
        if (i == 0 && !secret.empty()) result.text = std::string(479, 'x') + "🙂";
        if (i == 3) result.text = "timeout";
        if (i == 4) {
            result.kind = "command_execution";
            result.text = "$ cd build\ntrue";
        }
        if (i == 2) {
            result.attachments.push_back({logId, "test.log", "text/plain", {}, false});
            if (!secret.empty()) {
                result.title = secret;
                result.text.append(476 - result.text.size(), 'x').append(secret);
                result.attachments.front().name = secret;
            }
        }
        if (i == 3 && !secret.empty()) {
            result.text += " " + secret.substr(0, 12);
            result.textTruncated = true;
        }
        return result;
    }
    std::optional<SessionEventPage> sessionEvents(const SessionTarget& target, const SessionEventFilter& filter) override {
        if (!allowed(target)) return {};
        SessionEventPage page;
        unsigned i = static_cast<unsigned>(filter.afterPosition.value_or(-1) + 1);
        for (; i < 5 && page.items.size() < filter.limit; ++i) {
            auto event = item(i);
            if ((filter.kind.empty() || event.kind == filter.kind) && (!filter.matchText || filter.matchText(event)) &&
                session_read::inRange(event.time, filter.fromTime, filter.toTime))
                page.items.push_back(std::move(event));
        }
        if (i < 5) page.nextPosition = i - 1;
        return page;
    }
    std::optional<SessionEvent> sessionEvent(const SessionTarget& target, std::string_view id) override {
        if (!allowed(target)) return {};
        for (unsigned i = 0; i < 5; ++i)
            if (auto event = item(i); event.id == id) return event;
        return {};
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget& target) override {
        if (!allowed(target)) return {};
        return SessionCounts{11, 3};
    }
    std::optional<SessionAttachment> sessionAttachment(const SessionTarget& target, std::string_view id,
                                                       std::string_view attachment) override {
        if (!allowed(target) || id != failureId || attachment != logId || denyAttachment) return {};
        if (!secret.empty()) return SessionAttachment{logId, secret, "text/plain", std::string(8188, 'x') + secret + "🙂", false};
        return SessionAttachment{logId, "test.log", "text/plain", "fixture failed at the staging health check\n" + std::string(9000, 'x'),
                                 false};
    }
};

struct Agent : Model {
    Decision next;
    Json result;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        if (input.event.at("kind") == "intake")
            return {"delegate", {{"instruction", "Investigate staging"}, {"doneWhen", "Report failure"}}, "Investigate"};
        const auto& steps = input.event.at("turn");
        if (!steps.empty()) {
            result = steps.back().at("result");
            return {"reply", {{"text", "Checked."}}, "Respond with the evidence"};
        }
        check(std::ranges::find(input.tools, next.tool) != input.tools.end(), "tool is absent from the conversation");
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
        store = std::make_unique<Store>(dir / "state.db");
        runtime = std::make_unique<Runtime>(*store, model, t3, [](auto) { return std::string{}; });
    }
    Intake intake(std::string bot) {
        return {std::move(bot), "Investigate staging", env, "release", {{"conversationEnvironments", {{"Remote", otherEnv}}}}};
    }
    Json call(std::string tool, Json args, std::string bot = "alpha") {
        model.result = nullptr;
        model.next = {std::move(tool), std::move(args), "Inspect the requested investigation"};
        Conversations(*store).receive(localOwner, intake(std::move(bot)), std::to_string(++sequence),
                                      "Where did yesterday’s staging investigation fail?");
        runtime->runConversations();
        check(!model.result.is_null(), "conversation did not dispatch tool");
        return model.result;
    }
    void blockSession(std::string_view thread, bool blocked = false) {
        Policy{*store}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(env, thread).target, blocked);
    }
};

void acceptance() {
    Fixture f;
    const auto search = "flock_session_search";
    const auto events = "flock_session_events";
    const Json filters{{"query", "staging"},
                       {"project", "Release Pipeline"},
                       {"status", "failed"},
                       {"fromTime", "2026-11-21T00:00:00Z"},
                       {"toTime", "2026-11-22T00:00:00Z"},
                       {"limit", 1}};
    const Json empty{{"ok", true}, {"sessions", Json::array()}, {"nextCursor", nullptr}, {"truncated", false}};
    f.blockSession(staging, true);
    f.blockSession(second, true);
    f.blockSession("internal-shared-but-denied", true);
    Policy{*f.store}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(otherEnv, staging).target, true);
    check(f.call(search, filters) == empty && f.t3.reads == 0, "blocked Session contacted T3 or entered search");
    const auto missing = f.call(events, {{"sessionRef", "random"}, {"operation", "counts"}});
    check(missing == f.call(events, {{"sessionRef", staging}, {"operation", "counts"}, {"bot", "beta"}}) && missing.at("ok") == false &&
              f.t3.reads == 0,
          "manual thread ID exposed the blocked Session");
    f.blockSession(staging);
    f.blockSession(second);
    f.blockSession("internal-shared-but-denied");
    Policy{*f.store}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(otherEnv, staging).target, true);
    const auto found = f.call(search, filters);
    const auto searchCursor = found.at("nextCursor");
    check(found.at("sessions").size() == 1 && searchCursor.is_string(), "search pagination missing");
    const auto after = f.call(search, filters);
    check(after == found, "search references changed for the same filter");
    auto nextSearch = filters;
    nextSearch["cursor"] = searchCursor;
    const auto next = f.call(search, nextSearch);
    check(next.at("sessions").size() == 1 && next.at("nextCursor").is_null() &&
              next.at("sessions")[0].at("title") != found.at("sessions")[0].at("title"),
          "search pagination repeated a candidate");
    check(found.dump().find("internal-") == std::string::npos && next.dump().find("internal-") == std::string::npos,
          "search returned an internal thread identifier");
    const auto preview = f.call(search, {{"query", "regression failed"}});
    check(preview.at("sessions").size() == 2 && preview.at("sessions").at(0).at("matchExcerpt") == "The staging regression failed.",
          "message-only search lost an authorized local match");
    check(f.call(search, {{"query", "2"}}).at("sessions").size() == 1 && f.call(search, {{"query", std::string(201, 'x')}}) == empty,
          "Flock text filtering inherited T3 project-search limits");
    check(f.call(search, {{"fromTime", "2026-11-21T12:34:56.123000000Z"}, {"toTime", "2026-11-21T12:34:56.123Z"}}).at("sessions").size() ==
                  2 &&
              f.call(search, {{"toTime", "2026-11-21T12:34:56.122999999Z"}}) == empty &&
              f.call(search, {{"fromTime", "2026-02-29T00:00:00Z"}}).at("ok") == false && f.call(search, {{"limit", 21}}).at("ok") == false,
          "UTC range or limit validation is incorrect");

    const auto sessionRef = next.at("sessions").at(0).at("sessionRef");
    const auto otherRef = found.at("sessions").at(0).at("sessionRef");
    Json read{{"sessionRef", sessionRef}};
    const auto request = [&](std::string_view operation, Json args = Json::object(), std::string bot = "alpha") {
        args.update(read);
        args["operation"] = operation;
        return f.call(events, args, std::move(bot));
    };
    const auto first = request("list", {{"limit", 2}});
    const auto cursor = first.at("nextCursor");
    const auto secondPage = request("list", {{"cursor", cursor}, {"limit", 2}});
    check(first.at("events").size() == 2 && cursor.is_string() && secondPage.at("events").size() == 2 &&
              secondPage.at("events")[0].at("kind") == "command_execution",
          "ordered event pagination failed");
    const auto foundEvent = request("search", {{"kind", "command_execution"}, {"query", "FAILED"}, {"fromTime", timestamp}});
    const auto eventRef = foundEvent.at("events").at(0).at("eventRef");
    check(foundEvent.at("events").size() == 1 && eventRef == secondPage.at("events")[0].at("eventRef"), "filtered event search failed");
    const auto detail = request("read_event", {{"eventRef", eventRef}});
    const auto& event = detail.at("event");
    const auto attachmentRef = event.at("attachments").at(0).at("attachmentRef");
    check(event.at("text") == "$ cd build\nctest --output-on-failure\nTest 7 failed" && !event.contains("command") &&
              !event.contains("output") && !detail.at("truncated").get<bool>() && detail.dump().find("internal-") == std::string::npos,
          "failed event details or reference scope incorrect");
    const auto last = request("list", {{"limit", 5}}).at("events");
    const auto quiet = request("read_event", {{"eventRef", last.at(4).at("eventRef")}}).at("event");
    check(quiet.at("text") == "$ cd build\ntrue" && !quiet.contains("output"), "multiline command lines were reported as output");
    check(request("counts", {{"limit", 1}, {"kind", "error"}}).at("counts") == Json{{"messages", 11}, {"runs", 3}},
          "counts were scoped to a page or filter");
    check(request("counts", {{"bot", "alpha"}}, "beta") == missing &&
              f.call(events, {{"sessionRef", otherRef}, {"operation", "read_event"}, {"eventRef", eventRef}}) == missing &&
              f.call(events, {{"sessionRef", otherRef}, {"operation", "get_attachment"}, {"attachmentRef", attachmentRef}}) == missing,
          "references crossed a Bot or Session boundary");
    check(request("list", {{"cursor", cursor}, {"kind", "error"}}) == missing, "cursor reused with different filters");
    const auto missingAttachment = request("get_attachment", {{"attachmentRef", "missing"}});
    const auto fetchLog = [&] { return request("get_attachment", {{"attachmentRef", attachmentRef}}); };
    const auto log = fetchLog();
    check(log.at("attachment").at("content").get<std::string>().starts_with("fixture failed") &&
              log.at("attachment").at("content").get<std::string>().size() == 8192 && log.at("truncated") == true,
          "attachment content or truncation was lost");
    Policy{*f.store}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(otherEnv, staging).target, false);
    const auto acrossEnvironments = f.call(search, Json::object()).at("sessions");
    const auto remoteRef = acrossEnvironments.back().at("sessionRef");
    check(acrossEnvironments.size() == 3 && remoteRef != sessionRef &&
              f.call(events, {{"sessionRef", remoteRef}, {"operation", "read_event"}, {"eventRef", eventRef}}).dump() == missing.dump() &&
              f.call(events, {{"sessionRef", remoteRef}, {"operation", "list"}, {"cursor", cursor}}).dump() == missing.dump() &&
              f.call(events, {{"sessionRef", remoteRef}, {"operation", "get_attachment"}, {"attachmentRef", attachmentRef}}).dump() ==
                  missingAttachment.dump(),
          "references crossed environments for the same T3 thread identifier");
    Policy{*f.store}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(otherEnv, staging).target, true);
    auto tampered = sessionRef.get<std::string>();
    tampered.back() = tampered.back() == 'a' ? 'b' : 'a';
    check(f.call(events, {{"sessionRef", tampered}, {"operation", "counts"}}).dump() == missing.dump() &&
              f.call(events, {{"operation", "counts"}}).dump() == missing.dump() &&
              request("list", {{"cursor", std::string(129, 'x')}}).dump() == missing.dump() &&
              request("get_attachment", {{"attachmentRef", std::string(129, 'x')}}).dump() == missingAttachment.dump(),
          "invalid or missing references produced a distinct response");
    f.restart();
    check(fetchLog() == log && request("list", {{"cursor", cursor}, {"limit", 2}}) == secondPage && f.call(search, nextSearch) == next,
          "restart lost scoped references or read access");

    const auto task = Tasks(*f.store).submit(f.intake("beta"));
    for (int i = 0; i < 8 && Tasks(*f.store).task(task).task.state != TaskState::running; ++i) f.runtime->tick();
    const auto running = taskJson(Tasks(*f.store).task(task));
    check(running.at("state") == "running" && f.t3.writes.size() == 1, "foreign task did not start");
    const auto foreign = Resource::sessionRead(env, "foreign-thread");
    check(Policy{*f.store}.allows("beta", foreign) && Policy{*f.store}.allows("alpha", foreign),
          "Full did not allow Session reads for both Bots");
    check(f.call(search, {{"query", "Foreign"}}, "beta").at("sessions").size() == 1, "owned Session missing without a grant");
    auto outside = f.intake("outside");
    outside.projectId = "elsewhere";
    f.model.next = {search, {{"query", "Foreign"}}, "Check configured project"};
    Conversations(*f.store).receive(localOwner, outside, "outside-project", "Search my project");
    f.runtime->runConversations();
    check(f.model.result.at("sessions").empty(), "Search included a recorded run from a different project");
    f.t3.denied["foreign-thread"] = true;
    check(f.call(search, {{"query", "Foreign"}}, "beta") == empty, "T3 denial did not override Session ownership");
    f.t3.denied["foreign-thread"] = false;
    f.blockSession("foreign-thread");
    for (auto tool : {"message_session", "interrupt_session", "send_to_task", "cancel_task"}) {
        const auto outcome = f.call(
            tool, {{"taskId", task}, {"generation", running.at("generation")}, {"text", "Deliver this"}, {"instruction", "Deliver this"}});
        check(outcome.at("ok") == false, "Session read enabled a write or interruption");
    }
    check(f.t3.writes.size() == 1 && !f.t3.stops, "write tool bypassed task ownership");

    f.t3.denied[staging] = true;
    check(request("list", {{"cursor", cursor}, {"limit", 2}}) == missing && request("counts") == missing &&
              fetchLog() == missingAttachment && f.call(search, nextSearch) == empty,
          "T3 denial did not override the current read policy");
    check(request("list", {{"fromTime", "invalid"}}).dump() ==
                  f.call(events, {{"operation", "list"}, {"sessionRef", "missing"}, {"fromTime", "invalid"}}).dump() &&
              request("unknown").dump() == f.call(events, {{"operation", "unknown"}, {"sessionRef", "missing"}}).dump(),
          "request validation depended on whether a Session existed");
    f.t3.denied[staging] = false;
    f.blockSession(second, true);
    auto missingSearch = nextSearch;
    missingSearch["cursor"] = "missing";
    check(f.call(search, nextSearch).dump() == f.call(search, missingSearch).dump() && f.call(search, nextSearch) == missing,
          "revoked search cursor still returned a page");

    f.t3.onRead = [&] {
        Store writer(f.dir / "state.db");
        Policy{writer}.changeNeverRule(localOwner, Action::session_read, Resource::sessionRead(env, staging).target, true);
    };
    check(request("read_event", {{"eventRef", eventRef}}) == missing, "revocation during an upstream read was ignored");
    f.t3.onRead = {};
    check(request("list", {{"cursor", cursor}, {"limit", 2}}) == missing && request("read_event", {{"eventRef", eventRef}}) == missing &&
              fetchLog() == missingAttachment && request("counts") == missing,
          "revoked Session references still exposed evidence");
    f.blockSession("foreign-thread", true);
    check(f.call(search, filters) == empty, "revoked or unauthorized Sessions returned to search");
}

void catalogResultsAreDeduplicatedAcrossRecordedBatches() {
    Fixture f;
    f.t3.overlappingCatalog = true;
    Tasks(*f.store).submit(f.intake("alpha"));
    {
        sqlite3* raw = nullptr;
        check(sqlite3_open(f.store->path().c_str(), &raw) == SQLITE_OK, "could not open fixture database");
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, sqlite3_close);
        check(sqlite3_exec(writer.get(), R"SQL(
WITH RECURSIVE numbers(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM numbers WHERE n<65)
INSERT INTO outbox(id,task_id,phase,tool,environment_id,project_id,instruction,data)
SELECT printf('fixture-%02d',n),tasks.id,'accepted','delegate','desktop','project','',
       '{"target":null,"questionId":"","answers":{},"instructions":[],"contextIds":{},"sourceResultRefs":[],"chargesBudget":false}'
FROM numbers CROSS JOIN tasks;
INSERT INTO runs(action_id,task_id,environment_id,thread_id,run_id,link)
SELECT id,task_id,'desktop',CASE WHEN id='fixture-65' THEN 'internal-staging-thread' ELSE id END,id,''
FROM outbox;
)SQL",
                           nullptr, nullptr, nullptr) == SQLITE_OK,
              "could not seed recorded Sessions");
    }
    const auto search = "flock_session_search";
    const auto first = f.call(search, Json::object());
    check(first.at("sessions").empty() && first.at("nextCursor").is_string(), "recorded batch did not advance");
    const auto catalog = f.call(search, {{"cursor", first.at("nextCursor")}});
    check(catalog.at("sessions").size() == 1 && catalog.at("nextCursor").is_string(),
          "overlapping Session was not returned by the catalog");
    f.restart();
    const auto recorded = f.call(search, {{"cursor", catalog.at("nextCursor")}});
    check(recorded.at("sessions").empty() && recorded.at("nextCursor").is_null(),
          "recorded batch returned a Session already emitted by the catalog");
}

void catalogResumesAfterPartialDiscovery() {
    Fixture f;
    f.t3.pagedCatalog = true;
    const auto first = f.call("flock_session_search", Json::object());
    check(first.at("sessions").size() == 1 && first.at("truncated") == true && first.at("nextCursor").is_string(),
          "Partial catalog lost its results or continuation");
    f.restart();
    const auto secondPage = f.call("flock_session_search", {{"cursor", first.at("nextCursor")}});
    check(secondPage.at("sessions").size() == 1 && secondPage.at("nextCursor").is_null() &&
              secondPage.at("sessions").front().at("title") == "Staging investigation 2" && f.t3.catalogOffsets == std::vector<Id>{0, 1000},
          "Catalog restarted at offset zero");
}

void connectedSearchRespectsCurrentAccess() {
    Fixture f;
    f.t3.nativeCatalog = true;
    T3Connections connections(*f.store);
    connections.connect(env, localOwner.id, 48120);
    const auto search = "flock_session_search";
    const auto events = "flock_session_events";
    const Json filters{{"project", "Other Project"}};
    check(f.call(search, filters).at("sessions").empty(), "an unconnected environment entered the search");

    connections.connect(otherEnv, localOwner.id, 48120);
    const auto found = f.call(search, filters);
    check(found.at("sessions").size() == 1 && found.at("sessions").front().at("title") == "Native investigation",
          "connected search remained limited to the Bot routing project");
    check(found.at("sessions").front().at("threadId") == nativeSession && found.at("sessions").front().at("environmentId") == otherEnv,
          "connected search did not return the Session identifiers");
    const auto reference = found.at("sessions").front().at("sessionRef");
    const Json read{{"sessionRef", reference}, {"operation", "counts"}};
    const Json work{{"sessionRef", reference}, {"request", "Continue the investigation"}};
    check(f.call(events, read).at("ok") == true, "connected Session reference could not be read");

    auto& denied = f.t3.denied[nativeSession];
    denied = true;
    check(f.call(search, filters).at("sessions").empty() && f.call(events, read).at("ok") == false &&
              f.call("start_task", work).at("ok") == false,
          "upstream denial did not revoke a discovered Session");
    denied = false;

    Policy policy{*f.store};
    policy.changeNeverRule(localOwner, Action::environment_inspect, otherEnv, true);
    check(f.call(search, filters).at("sessions").empty() && f.call(events, read).at("ok") == false,
          "a Never rule did not revoke connected search access");
    policy.changeNeverRule(localOwner, Action::environment_inspect, otherEnv, false);

    policy.setMember(localOwner, "alpha", "guest", true);
    check(f.call(search, filters).at("sessions").empty() && f.call(events, read).at("ok") == false &&
              f.call("start_task", work).at("ok") == false,
          "sharing a Bot retained private connected Session access");
    policy.setMember(localOwner, "alpha", "guest", false);
    check(f.call(events, read).at("ok") == true, "removing the share did not restore private scope");

    connections.disconnect(otherEnv);
    const int reads = f.t3.reads;
    check(f.call(events, read).at("ok") == false && f.call("start_task", work).at("ok") == false && f.t3.reads == reads,
          "a disconnected environment remained reachable using a saved reference");
}

void approvedCallSurvivesReadPreflightContention() {
    Fixture f;
    f.model.next = {"flock_session_search", {{"query", "regression"}}, "Find the report"};
    auto intake = f.intake("alpha");
    intake.config["modelCallLimit"] = 1;
    const auto receipt = Conversations(*f.store).receive(localOwner, intake, "preflight-busy", "Look for the failure");
    f.runtime->runConversations();
    const auto paused = Conversations(*f.store).pausedConversations("alpha");
    check(paused.size() == 1 && f.model.result.is_null(), "turn did not pause after reading evidence");
    const Id pauseId = paused.front().at("messageId");
    Inbox(*f.store).decideInbox(
        localOwner, {"alpha", InboxKind::conversation_model_budget, receipt.at("messageId"), pauseId, 0, 0, pauseId}, "approve");
    sqlite3* raw = nullptr;
    check(sqlite3_open(f.store->path().c_str(), &raw) == SQLITE_OK, "could not open the second writer");
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw, sqlite3_close);
    bool locked = false;
    f.t3.onRead = [&] {
        if (!std::exchange(locked, true))
            check(sqlite3_exec(writer.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK, "lock writer");
    };
    bool contended = false;
    try {
        f.runtime->runConversations();
    } catch (const std::runtime_error& error) {
        contended = std::string_view(error.what()) == "database is locked";
    }
    check(locked && contended && f.model.result.is_null(), "pre-provider write contention was not exercised");
    check(sqlite3_exec(writer.get(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK, "could not release writer");
    f.t3.onRead = {};
    f.runtime->runConversations();
    const auto after = Conversations(*f.store).pausedConversations("alpha");
    check(!f.model.result.is_null() && after.empty(), "Conversation lost its approved call before model dispatch");
}

void redactionAndResumedHistory() {
    Fixture f;
    f.blockSession(staging);
    Policy policy{*f.store};
    const auto secret = "synthetic-session-credential";
    const auto key = policy.requestSecret("beta", {"beta-fixture"});
    policy.submitSecret({"root", true}, "beta", key, secret);
    f.t3.secret = secret;

    const auto found = f.call("flock_session_search", {{"query", "regression"}});
    const auto& row = found.at("sessions").front();
    check(found.dump().find("synt") == std::string::npos && row.at("matchExcerpt") == "The staging regression failed. [redacted]" &&
              found.at("truncated") == true,
          "search exposed a protected value across an excerpt boundary");
    const auto ref = row.at("sessionRef");
    const auto events = f.call("flock_session_events", {{"sessionRef", ref}, {"operation", "list"}});
    const auto& failed = events.at("events").at(2);
    const auto& error = events.at("events").at(3);
    check(events.dump().find("synt") == std::string::npos && failed.at("title") == "[redacted]" &&
              events.at("events").front().at("excerpt").get<std::string>().size() == 479 && error.at("excerpt") == "timeout [redacted]" &&
              error.at("truncated") == true,
          "list exposed event metadata or a truncated protected value");
    const auto detail =
        f.call("flock_session_events", {{"sessionRef", ref}, {"operation", "read_event"}, {"eventRef", error.at("eventRef")}});
    check(detail.at("event").at("error") == "timeout [redacted]", "event error was not redacted");
    const Json attachment{
        {"sessionRef", ref}, {"operation", "get_attachment"}, {"attachmentRef", failed.at("attachments").front().at("attachmentRef")}};
    const auto log = f.call("flock_session_events", attachment);
    check(log.at("attachment").at("content").get<std::string>().size() == 8192 && log.at("attachment").at("name") == "[redacted]" &&
              log.dump().find("synt") == std::string::npos && log.at("truncated") == true,
          "attachment exposed a protected value at the byte limit");

    const auto missing = f.call("flock_session_events", {{"sessionRef", "unknown"}, {"operation", "counts"}});
    const auto pauseThenRevoke = [&](Decision step, const std::function<void()>& revoke) {
        f.model.next = std::move(step);
        f.model.result = nullptr;
        auto intake = f.intake("alpha");
        intake.config["modelCallLimit"] = 1;
        const auto receipt = Conversations(*f.store).receive(localOwner, intake, std::to_string(++f.sequence), "Look for the failure");
        f.runtime->runConversations();
        const auto paused = Conversations(*f.store).pausedConversations("alpha");
        check(paused.size() == 1 && f.model.result.is_null(), "turn did not pause after reading evidence");
        revoke();
        f.restart();
        const Id pauseId = paused.front().at("messageId");
        Inbox(*f.store).decideInbox(
            localOwner, {"alpha", InboxKind::conversation_model_budget, receipt.at("messageId"), pauseId, 0, 0, pauseId}, "approve");
        f.runtime->runConversations();
        check(f.model.result.dump() == missing.dump(), "reopened turn exposed inaccessible cached evidence");
    };
    pauseThenRevoke({"flock_session_events", attachment, "Read the log"}, [&] { f.t3.denyAttachment = true; });
    pauseThenRevoke({"flock_session_search", Json::object(), "Find staging"}, [&] { f.t3.denied[staging] = true; });
    f.t3.denied[staging] = false;
    pauseThenRevoke({"flock_session_search", Json::object(), "Find staging"}, [&] { f.blockSession(staging, true); });
}
}

int main() {
    try {
        acceptance();
        catalogResumesAfterPartialDiscovery();
        catalogResultsAreDeduplicatedAcrossRecordedBatches();
        connectedSearchRespectsCurrentAccess();
        redactionAndResumedHistory();
        approvedCallSurvivesReadPreflightContention();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
