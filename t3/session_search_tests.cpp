#include "flock/policy.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"

#include <algorithm>

using namespace flockv0;

namespace {
void check(bool pass, const char* reason) {
    if (!pass) throw std::runtime_error(reason);
}

struct SearchModel final : Model {
    Decision next;
    Json result;

    ModelTurn decide(const ModelInput& input) override {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return {{next}};
        result = steps.back().at("result");
        return {{{"reply",{{"text","Checked."}},"Respond with the evidence"}}};
    }
};

struct SearchConversation {
    Store store;
    SearchModel model;
    Runtime runtime;
    int sequence = 0;

    SearchConversation(T3& t3, const std::filesystem::path& database)
        : store(database), runtime(store,model,t3,[](std::string_view) { return std::string{}; }) {}

    void blockSession(std::string_view thread, bool blocked = false) {
        store.policy().changeNeverRule(localOwner,Action::session_read,Resource::sessionRead("env-1",thread).target,blocked);
    }

    Json call(std::string tool, Json args, std::string bot = "release") {
        const Intake intake{std::move(bot),"Investigate staging","env-1","project",Json::object()};
        model.result = nullptr;
        model.next = {std::move(tool),std::move(args),"Inspect the requested investigation"};
        store.receive(localOwner,intake,std::to_string(++sequence),"Check the investigation.");
        runtime.runConversations();
        return model.result;
    }

    Json search(Json args, std::string bot = "release") {
        return call("flock_session_search",std::move(args),std::move(bot));
    }
};
}

void deletedSessionSearch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    SearchConversation conversation(t3,database);
    const auto call = [&](std::string tool, Json args) {
        return conversation.call(std::move(tool),std::move(args));
    };
    const auto found = call("flock_session_search",{{"query","Former investigation"}});
    check(found.at("ok") == true && found.at("sessions").size() == 1,"could not reference the Session before deletion");
    const Json counts{{"operation","counts"},{"sessionRef",found.at("sessions").at(0).at("sessionRef")}};
    check(call("flock_session_events",counts).at("ok") == true,"could not read the Session before deletion");

    const auto threadError = [&](const char* code, const char* message) {
        fixture({{"threadReadErrors",{{"deleted",{{"code",code},{"message",message}}}}}});
    };
    threadError("capability_denied","Read scope required.");
    const auto denied = call("flock_session_events",counts);
    check(denied.at("ok") == false,"T3 read denial was ignored");

    threadError("invalid_request","The thread was not found.");
    const auto surviving = call("flock_session_search",Json::object());
    check(surviving.at("ok") == true && surviving.at("sessions").size() == 1 &&
          surviving.at("sessions").at(0).at("title") == "Staging investigation",
          "deleted Session blocked a readable search result");
    check(call("flock_session_events",counts).dump() == denied.dump(),"deleted and inaccessible Sessions differed");

    threadError("invalid_request","The view was not supported.");
    check(call("flock_session_search",Json::object()).at("ok") == false,"unrelated invalid request was suppressed");
    fixture({{"threadReadErrors",Json::object()}});
}

void boundedSessionSearch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture,
                          const std::function<Json(std::string_view)>& effects) {
    SearchConversation conversation(t3,database);
    fixture({{"scenario","sparse-session-search"},{"missingSearchThreads",{"candidate-1063"}},
             {"searchTitles",{{"candidate-1064","Staging failure A"},{"candidate-1128","Staging failure B"},
                              {"candidate-1129","Staging failure C"}}}});

    const Json query{{"query","staging failure"}};
    const auto before = effects("t3_thread_read").size();
    const auto first = conversation.search(query);
    const auto reads = effects("t3_thread_read");
    check(first.at("ok") == true && first.at("sessions").empty() && first.at("nextCursor").is_string() &&
          first.at("truncated") == false,"a sparse empty page did not return continuation");
    check(reads.size() == before+64 && reads.back().at("threadId") == "candidate-1063",
          "a search page read beyond its candidate limit");
    check(first.dump().find("candidate-") == std::string::npos,"the opaque scan cursor exposed a thread ID");
    auto resume = query;
    resume["cursor"] = first.at("nextCursor");
    auto wrongFilter = resume;
    wrongFilter["status"] = "failed";
    check(conversation.search(wrongFilter).at("ok") == false &&
          conversation.search(resume,"other-bot").at("ok") == false,"scan cursor crossed filters or Bots");

    const auto second = conversation.search(resume);
    check(second.at("sessions").size() == 1 && second.at("sessions").at(0).at("title") == "Staging failure A" &&
          second.at("nextCursor").is_string() && second.at("nextCursor") != first.at("nextCursor"),
          "continuing an empty page skipped a match or failed to advance");
    check(effects("t3_thread_read").at(reads.size()).at("threadId") == "candidate-1064",
          "continuation did not resume immediately after the last scanned candidate");
    resume["cursor"] = second.at("nextCursor");
    conversation.blockSession("candidate-1128",true);
    const auto last = conversation.search(resume);
    check(last.at("sessions").size() == 1 && last.at("sessions").at(0).at("title") == "Staging failure C" &&
          last.at("nextCursor").is_null(),"continuation included a blocked Session or missed the end of the search");
    conversation.blockSession("candidate-1128");
    const auto restored = conversation.search(resume);
    check(restored.at("sessions").size() == 2 && restored.at("sessions").at(0).at("title") == "Staging failure B" &&
          restored.at("sessions").at(1).at("title") == "Staging failure C" && restored.at("nextCursor").is_null(),
          "continuation did not preserve order or reflect the current Session filter");
}

void sessionSearchDeadline(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    SearchConversation conversation(t3,database);
    fixture({{"slowSearchThread","deadline-2"},{"searchTitles",{{"deadline-2","Staging failure A"},
                                                               {"deadline-3","Staging failure B"}}}});
    const Json query{{"query","staging failure"}};
    const auto start = std::chrono::steady_clock::now();
    const auto partial = conversation.search(query);
    const auto elapsed = std::chrono::steady_clock::now()-start;
    check(elapsed < std::chrono::milliseconds(3100),"slow T3 calls outlasted the shared search deadline");
    check(partial.at("ok") == true && partial.at("sessions").empty() && partial.at("nextCursor").is_string(),
          "a slow candidate discarded scan progress");

    auto resume = query;
    resume["cursor"] = partial.at("nextCursor");
    const auto deferredStart = std::chrono::steady_clock::now();
    const auto deferred = conversation.search(resume);
    check(std::chrono::steady_clock::now()-deferredStart < std::chrono::milliseconds(3100),
          "a candidate that supplied no evidence was read again before the next model call");
    check(deferred.at("ok") == true && deferred.at("truncated") == true && deferred.at("sessions").empty() &&
          deferred.at("nextCursor").is_string(),"a repeatedly slow candidate did not return an incomplete continuation");
    auto pastSlow = query;
    pastSlow["cursor"] = deferred.at("nextCursor");
    const auto later = conversation.search(pastSlow);
    check(later.at("sessions").size() == 1 && later.at("sessions").at(0).at("title") == "Staging failure B" &&
          later.at("nextCursor").is_null(),"a persistently slow candidate blocked later matches");
    fixture({{"slowSearchThread",nullptr}});
    const auto resumed = conversation.search(resume);
    check(resumed.at("sessions").size() == 2 && resumed.at("nextCursor").is_null() &&
          resumed.at("sessions").at(0).at("title") == "Staging failure A" &&
          resumed.at("sessions").at(1).at("title") == "Staging failure B",
          "the timed-out candidate was skipped or reordered on retry");

    auto matchedPage = query;
    matchedPage["limit"] = 1;
    const auto firstMatch = conversation.search(matchedPage);
    check(firstMatch.at("sessions").size() == 1 && firstMatch.at("nextCursor").is_string(),
          "could not create a matched-result cursor");
    matchedPage["cursor"] = firstMatch.at("nextCursor");
    fixture({{"searchReadDelays",{{"deadline-2",1.15},{"deadline-3",0.9}}}});
    const auto afterSlowAnchor = conversation.search(matchedPage);
    check(afterSlowAnchor.at("ok") == true && afterSlowAnchor.at("sessions").size() == 1 &&
          afterSlowAnchor.at("sessions").at(0).at("title") == "Staging failure B" &&
          afterSlowAnchor.at("nextCursor").is_null() && afterSlowAnchor.at("truncated") == false,
          "a slow but readable anchor exhausted the next candidate's budget");

    fixture({{"searchReadDelays",Json::object()},{"missingSearchThreads",{"deadline-2"}}});
    check(conversation.search(matchedPage).at("ok") == false,"matched-result cursor accepted an unreadable anchor");
    fixture({{"scenario","session-read"},{"missingSearchThreads",Json::array()}});
}


void sessionMessageWake(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture,
                        const std::function<Json(std::string_view)>& effects) {
    struct ModelFixture final : Model {
        Decision next;
        Json result;
        std::vector<Json> wakes, reads;
        ModelTurn decide(const ModelInput& input) override {
            const auto& steps = input.event.at("turn");
            if (const auto wake = input.event.find("sessionWake"); wake != input.event.end()) {
                if (steps.empty()) {
                    wakes.push_back(*wake);
                    return {{{"flock_session_events",{{"operation","counts"},{"environmentId",wake->at("environmentId")},
                                                      {"threadId",wake->at("threadId")}},"Read the Session after waking"}}};
                }
                reads.push_back(steps.back().at("result"));
            } else if (steps.empty()) {
                check(std::ranges::find(input.tools,next.tool) != input.tools.end(),"Session messaging was not offered");
                return {{next}};
            } else result = steps.back().at("result");
            return {{{"reply",{{"text","Checked."}},"Respond after checking"}}};
        }
    } model;
    fixture({{"scenario","running"},{"sessionWakeFixture",true},{"botRunStatus","queued"}});
    auto store = std::make_unique<Store>(database);
    auto runtime = std::make_unique<Runtime>(*store,model,t3,[](std::string_view) { return std::string{}; });
    const auto configureIdentity = [&] {
        store->setWebIdentityResolver([](std::string_view person) -> std::optional<bool> {
            if (person == localOwner.id) return true;
            return {};
        });
    };
    configureIdentity();
    const auto restart = [&] {
        runtime.reset();
        store = std::make_unique<Store>(database);
        configureIdentity();
        runtime = std::make_unique<Runtime>(*store,model,t3,[](std::string_view) { return std::string{}; });
    };
    T3Connections(*store).connect("env-1",localOwner.id,3210);
    const Intake intake{"release","Inspect staging","env-1","project",{{"botDisplayName","Ernest"}}};
    const auto receive = [&](std::string_view id) {
        return store->receiveWeb(localOwner,intake,id,"Send the follow-up.");
    };
    model.next = {"flock_session_search",Json::object(),"Find the Session"};
    receive("find");
    runtime->runConversations();
    check(model.result.is_object(),"Web conversation did not call Session search");
    const auto session = model.result.at("sessions").at(0);
    const auto thread = session.at("threadId").get<std::string>();
    check(thread == "thread" && session.at("environmentId") == "env-1","Search did not return the Session identifiers");
    model.next = {"message_session",{{"environmentId",session.at("environmentId")},{"threadId",thread},
                                    {"text","Check the fixture output"}},"Queue the follow-up"};
    const auto resource = Resource::sessionRead("env-1",thread).target;
    const auto sends = [&] { return effects("t3_thread_send").size(); };
    const auto before = sends();
    store->policy().changeNeverRule(localOwner,Action::t3_send,resource,true);
    receive("never");
    runtime->runConversations();
    check(sends() == before && model.result.at("ok") == false,"Never permitted a Session send");
    store->policy().changeNeverRule(localOwner,Action::t3_send,resource,false);
    store->policy().setMode(localOwner,"supervised");
    const auto origin = receive("supervised");
    runtime->runConversations();
    const auto item = store->inbox(localOwner).at(0);
    check(sends() == before && item.at("status") == "pending","Supervised sent before approval");
    restart();
    const InboxTarget approval{item.at("botId"),InboxKind::policy_approval,item.at("itemId"),item.at("waitId"),
                               item.at("taskId"),item.at("targetRevision"),item.at("targetGeneration")};
    store->decideInbox(localOwner,approval,"allow_once");
    runtime->runConversations();
    if (sends() != before + 1 || model.result.value("delivery","") != "queued")
        throw std::runtime_error("Approved Session send did not queue: " + model.result.dump());
    const auto wire = effects("t3_thread_send").back();
    const auto requestId = "flock-turn-" + std::to_string(origin.at("messageId").get<Id>()) + "-1";
    check(wire.at("mode") == "queue" && wire.at("threadId") == thread &&
          wire.at("message") == "Ernest: Check the fixture output" &&
          wire.at("clientRequestId") == requestId,"Wrong Session send arguments");
    check(receive("supervised").at("messageId") == origin.at("messageId"),"Duplicate intake created another turn");
    fixture({{"personFinished",true}});
    runtime->runConversations();
    check(model.wakes.empty() && sends() == before + 1,"The person's completion woke the Bot or resent the step");
    fixture({{"differentWaitRun",true}});
    runtime->runConversations();
    check(model.wakes.empty(),"A different Run's wait response woke the Bot");
    restart();
    fixture({{"differentWaitRun",false},{"botRunStatus","completed"}});
    runtime->runConversations();
    check(model.wakes.size() == 1 && model.wakes.back().at("reason") == "finished" &&
          model.reads.back().at("ok") == true,"Restart lost the Bot Run wake or its Session history");
    restart();
    runtime->runConversations();
    check(model.wakes.size() == 1,"Restart repeated an already delivered wake");

    store->policy().setMode(localOwner,"full");
    fixture({{"botRunStatus","queued"}});
    receive("full");
    runtime->runConversations();
    check(sends() == before + 2 && model.result.at("ok") == true,"Full did not send");
    fixture({{"botRunStatus","running"},{"botQuestion",true}});
    runtime->runConversations();
    fixture({{"botRunStatus","completed"},{"botQuestion",false}});
    runtime->runConversations();
    check(model.wakes.size() == 2 && model.wakes.back().at("reason") == "waiting_for_person",
          "A pending user question did not wake exactly once");
    fixture({{"sessionWakeFixture",false}});
}
