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
}

void deletedSessionSearch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    Store store(database);
    SearchModel model;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; });
    for (const auto* thread : {"deleted","manual"})
        store.access().resourceGrant({"root",true},"release",Scope::sessionRead("env-1",thread),GrantChange::grant);

    const Intake intake{"release","Investigate staging","env-1","project",Json::object(),{},0};
    int sequence = 0;
    const auto call = [&](std::string tool, Json args) {
        model.result = nullptr;
        model.next = {std::move(tool),std::move(args),"Inspect the requested investigation"};
        store.receive(intake,std::to_string(++sequence),"Check the investigation.");
        runtime.runConversations();
        return model.result;
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
