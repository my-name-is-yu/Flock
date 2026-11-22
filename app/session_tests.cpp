#include "model.hpp"
#include "flock/access.hpp"

#include <algorithm>

namespace flockv0::app {
namespace {
void check(bool pass, std::string_view reason) {
    if (!pass) throw std::runtime_error(std::string(reason));
}
}

void sessionConversationDispatch(Store& store, const Json& config) {
    const auto shared = Scope::sessionRead("env-canonical","manual-staging");
    struct HistoryT3 final : T3 {
        int sends = 0;
        std::string authGeneration(const std::string&,const Json&) override { return "test-auth"; }
        Delivery send(const Command&) override { ++sends; return {}; }
        Delivery reconcile(const Command&) override { throw std::runtime_error("Unexpected reconcile"); }
        Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
        void interrupt(const std::string&,const Json&,const RunRef&) override {}
        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool includeMessages,
                                                      SessionReadDeadline) override {
            if (target.threadId.starts_with("background-")) return {};
            check(target.environmentId == "env-canonical" && target.threadId == "manual-staging",
                  "search lost the authorized target");
            return SessionMetadata{"Staging investigation","Release","project-canonical","failed",
                                   "2026-11-21T00:00:00Z","2026-11-21T00:00:00Z",{},includeMessages};
        }
        std::optional<SessionEventPage> sessionEvents(const SessionTarget& target,
                                                     const SessionEventFilter& filter) override {
            check(target.threadId == "manual-staging" && filter.limit == 1,"Session reference or limit was not applied");
            SessionEvent evidence;
            evidence.id = filter.afterPosition ? "failed" : "start";
            evidence.text = filter.afterPosition ? "staging failed" : "staging started";
            return SessionEventPage{{evidence},filter.afterPosition ? std::nullopt : std::optional<Id>(0),false};
        }
    } t3;
    struct HistoryModel final : Model {
        Store& store;
        const Scope& shared;
        bool paged = false, revoked = false;
        Json sessionRef, emptyPage;
        HistoryModel(Store& store, const Scope& shared) : store(store), shared(shared) {}
        ModelTurn decide(const ModelInput& input) override {
            const auto request = responseRequest(input);
            const auto& steps = input.event.at("turn");
            std::string tool = "flock_session_search";
            Json args{{"query","staging"},{"reason","Find the shared investigation"}};
            if (steps.size() == 1) {
                emptyPage = steps[0].at("result");
                check(emptyPage.at("sessions").empty() && emptyPage.at("nextCursor").is_string(),
                      "empty search page did not reach the model with its continuation");
                args["cursor"] = emptyPage.at("nextCursor");
            } else if (steps.size() == 2) {
                const auto& sessions = steps[1].at("result").at("sessions");
                sessionRef = sessions.at(0).at("sessionRef");
                tool = "flock_session_events";
                args = {{"sessionRef",sessionRef},{"operation","list"},{"limit",1},{"reason","Read the first event"}};
            } else if (steps.size() == 3) {
                tool = "flock_session_events";
                args = {{"sessionRef",sessionRef},{"operation","list"},{"limit",1},
                        {"cursor",steps[2].at("result").at("nextCursor")},{"reason","Read the next event"}};
            } else if (steps.size() == 4) {
                const auto& event = steps[3].at("result").at("events").at(0);
                paged = event.at("excerpt") == "staging failed";
                store.access().resourceGrant({"root",true},"release",shared,GrantChange::revoke);
                tool = "flock_session_events";
                args = {{"sessionRef",sessionRef},{"operation","read_event"},{"eventRef",event.at("eventRef")},
                        {"reason","Read details of the failure"}};
            } else if (steps.size() == 5) {
                const Json missing{{"ok",false},{"error","Session evidence not found"}};
                revoked = steps[0].at("result") == emptyPage &&
                    std::all_of(steps.begin()+1,steps.end(),[&](const Json& step) { return step.at("result") == missing; });
                tool = "reply";
                args = {{"text","The investigation is no longer readable."},{"reason","Explain the status"}};
            }
            return responseDecision(Json::array({{{"type","function_call"},{"namespace","flock"},{"status","completed"},
                                    {"name",tool},{"arguments",args.dump()}}}),request);
        }
    } model(store,shared);
    for (int i = 0; i < 64; ++i)
        store.access().resourceGrant({"root",true},"release",
            Scope::sessionRead("env-canonical","background-"+std::to_string(i)),GrantChange::grant);
    store.access().resourceGrant({"root",true},"release",shared,GrantChange::grant);
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
    store.receive(localOwner,{"release","Read staging","env-canonical","project-canonical",config},
                  "read-staging","How much work went into the staging investigation?");
    runtime.runConversations();
    check(model.paged && model.revoked && t3.sends == 0,"model schemas did not dispatch pagination and revocation");
}
}
