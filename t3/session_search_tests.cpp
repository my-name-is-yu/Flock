#include "flock/access.hpp"

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

    void share(std::string_view thread, GrantChange change = GrantChange::grant) {
        store.access().resourceGrant({"root",true},"release",Scope::sessionRead("env-1",thread),change);
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
    for (const auto* thread : {"deleted","manual"}) conversation.share(thread);
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
    for (int i = 0; i < 130; ++i) conversation.share("candidate-"+std::to_string(1000+i));
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
    conversation.share("candidate-1128",GrantChange::revoke);
    const auto last = conversation.search(resume);
    check(last.at("sessions").size() == 1 && last.at("sessions").at(0).at("title") == "Staging failure C" &&
          last.at("nextCursor").is_null(),"continuation used a revoked grant or missed the end of the search");
    conversation.share("candidate-1128");
    const auto restored = conversation.search(resume);
    check(restored.at("sessions").size() == 2 && restored.at("sessions").at(0).at("title") == "Staging failure B" &&
          restored.at("sessions").at(1).at("title") == "Staging failure C" && restored.at("nextCursor").is_null(),
          "continuation did not preserve order or see the live grant");
}

void sessionSearchDeadline(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    SearchConversation conversation(t3,database);
    for (const auto* thread : {"deadline-1","deadline-2","deadline-3"}) conversation.share(thread);
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
    const auto deferred = conversation.search(resume);
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
    fixture({{"scenario","session-read"}});
}
