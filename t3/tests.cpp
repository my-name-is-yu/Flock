#include "internal.hpp"
#include "flock/board_inbox.hpp"
#include "flock/participation.hpp"
#include "flock/policy.hpp"
#include "flock/session_read.hpp"

#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace flockv0;
using flockv0::t3detail::http;

void sessionMessageWake(T3&, const std::filesystem::path&, const std::function<void(Json)>&,
                        const std::function<Json(std::string_view)>&);
void deletedSessionSearch(T3&, const std::filesystem::path&, const std::function<void(Json)>&);
void boundedSessionSearch(T3&, const std::filesystem::path&, const std::function<void(Json)>&,
                          const std::function<Json(std::string_view)>&);
void sessionSearchDeadline(T3&, const std::filesystem::path&, const std::function<void(Json)>&);

namespace {
void check(bool pass, std::string_view reason) {
    if (!pass) throw std::logic_error(std::string(reason));
}

void lineageFallback(T3& t3, Command launch, const std::function<void(Json)>& fixture,
                     const std::function<Json(std::string_view)>& effects, const std::function<Json()>& sessions) {
    launch.authGeneration = t3.authGeneration(launch.environmentId,launch.config);
    launch.sessionOrigin = SessionOrigin{{"origin-environment","lineage-parent"},"Deployment planning"};
    const Json origin{{"environmentId","origin-environment"},{"threadId","lineage-parent"},{"title","Deployment planning"}};
    const auto launches = [&] { return effects("t3_thread_launch"); };
    const auto start = launches().size();
    launch.actionId = "lineage-supported";
    check(t3.send(launch).kind == Delivery::Kind::Accepted && launches().size() == start + 1 &&
          launches().back().at("delegatedFrom") == origin,"T3 did not receive the linked launch");

    for (const std::string mode : {"unsupported","unknown-delegated-from"}) {
        fixture({{"lineageLaunchError",mode}});
        launch.actionId = "lineage-" + mode;
        const auto before = launches().size();
        const auto count = sessions().size();
        const auto accepted = t3.send(launch);
        const auto attempts = launches();
        check(accepted.kind == Delivery::Kind::Accepted && !accepted.launchWarning.empty() &&
              attempts.size() == before + 2,"A confirmed rejection did not retry exactly once and report the limitation");
        auto withOrigin = attempts.at(before);
        check(withOrigin.at("delegatedFrom") == origin,"The first request lost its origin");
        withOrigin.erase("delegatedFrom");
        check(withOrigin == attempts.back() && withOrigin.at("clientRequestId") == launch.actionId,
              "The retry changed arguments beyond delegatedFrom");
        const auto replay = t3.send(launch);
        check(replay.kind == Delivery::Kind::Accepted && replay.run.threadId == accepted.run.threadId &&
              replay.run.runId == accepted.run.runId && sessions().size() == count + 1,
              "Replaying a rejected launch created another Session");
    }

    for (const std::string mode : {"invalid-params","tool","lost-response","fallback-fails"}) {
        fixture({{"lineageLaunchError",mode}});
        launch.actionId = "lineage-" + mode;
        const auto before = launches().size();
        const auto count = sessions().size();
        const auto failed = t3.send(launch);
        const bool attemptedFallback = mode == "fallback-fails";
        check(failed.kind == Delivery::Kind::Unknown && launches().size() == before + 1 + attemptedFallback &&
              failed.launchWarning.empty() != attemptedFallback,
              "Flock retried an unrelated error or attempted more than one fallback");
        check(sessions().size() == count + (mode == "lost-response" ? 1 : 0),
              "Unexpected Session creation after a launch error");
        if (mode == "lost-response") {
            const auto recovered = t3.reconcile(launch);
            check(recovered.kind == Delivery::Kind::Accepted &&
                  recovered.run.threadId == sessions().at(launch.actionId).at("threadId").get<std::string>() &&
                  launches().size() == before + 1,"Reconciliation missed the linked child or replayed an uncertain launch");
        }
    }
}

void sessionOriginLaunch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture,
                         const std::function<Json(std::string_view)>& effects) {
    const Json origin{{"environmentId","env-1"},{"threadId","lineage-parent"},{"title","Deployment planning"}};
    struct LaunchModel final : Model {
        Decision next;
        Json origin, result, tasks, invitation;
        int wakes = 0;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") != "conversation_turn")
                return {{{"delegate",{{"instruction","Check the fixture."},{"doneWhen","The fixture is checked."}},"Start work"}}};
            if (input.event.contains("sessionInvitation")) {
                invitation = input.event;
                return {{{"reply",{{"text","Joined the child Session."}},"Finish the invitation turn"}}};
            }
            const auto& steps = input.event.at("turn");
            if (steps.empty()) {
                origin = input.event.value("sessionOrigin",Json(nullptr));
                tasks = input.task.at("tasks");
                if (input.event.contains("sessionWake")) {
                    ++wakes;
                    auto args = input.event.at("sessionWake");
                    return {{{"flock_session_events",{{"operation","counts"},{"environmentId",args.at("environmentId")},
                                                     {"threadId",args.at("threadId")}},"Read the child after waking"}}};
                }
                return {{next}};
            }
            result = steps.back().at("result");
            return {{{"reply",{{"text","Checked."}},"Finish this turn"}}};
        }
    } model;
    fixture({{"lineageLaunchError","unsupported"}});
    Store store(database);
    const Person member{"session-member",false};
    store.setWebIdentityResolver([&](std::string_view id) -> std::optional<bool> {
        if (id == member.id) return false;
        return {};
    });
    store.policy().setMember(localOwner,"release",member.id,true);
    store.policy().setMember(localOwner,"triage",member.id,true);
    T3Connections connections(store);
    const auto credential = connections.connect("env-1",member.id,3210);
    const auto connection = connections.authenticate(credential);
    check(connection.has_value(),"Could not authenticate the Session integration");
    SessionParticipation participation(store,t3);
    const Intake intake{"release",{},"env-1","project",{{"botDisplayName","Ernest"}}};
    const SessionAsk start{"release","Start child work.","lineage-parent","lineage-ask"};
    const auto resolve = [&](auto,auto) { return intake; };
    const auto message = participation.ask(*connection,start,resolve);
    check(participation.ask(*connection,start,resolve) == message,"The same Session ask created another turn");
    store.policy().setMode(localOwner,"supervised");
    const RuntimeOptions options{.intake=[](std::string_view bot, std::string_view text) {
        return Intake{std::string(bot),std::string(text),"env-1","project"};
    }};
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
    model.next = {"start_task",{{"request","Check the release fixture."}},"Start work in a new Session"};
    runtime.runConversations();
    check(model.origin == origin && model.result.value("ok",false),"Session origin was not saved before starting the task");
    const Id taskId = model.result.at("taskId");
    check(store.task(taskId).at("sessionOrigin") == origin,"The task omitted its originating Session");

    const auto launches = [&] { return effects("t3_thread_launch"); };
    const auto before = launches().size();
    Json approval;
    for (int i = 0; i < 6 && approval.is_null(); ++i) {
        runtime.tick();
        for (const auto& item : store.inbox(member)) if (item.at("status") == "pending") approval = item;
    }
    check(!approval.is_null() && launches().size() == before,"The linked launch bypassed Supervised approval");
    store.decideInbox(member,{approval.at("botId"),InboxKind::policy_approval,approval.at("itemId"),approval.at("waitId"),
                             approval.at("taskId"),approval.at("targetRevision"),approval.at("targetGeneration")},"allow_once");
    for (int i = 0; i < 6 && launches().size() == before; ++i) runtime.tick();
    const auto attempts = launches();
    check(attempts.size() == before + 2 && attempts.at(before).at("delegatedFrom") == origin &&
          !attempts.at(before + 1).contains("delegatedFrom"),"The approved Session-origin task sent the wrong launch");
    const auto task = store.task(taskId);
    const auto thread = task.at("activeRun").at("threadId");
    check(task.contains("launchWarning") && std::ranges::any_of(store.history(taskId),[](const Json& entry) {
        return entry.at("kind") == "session_lineage_unavailable";
    }),"A launch fallback was not recorded on the task and in its journal");

    store.policy().setMode(localOwner,"full");
    const auto call = [&](const char* clientId, std::string tool, const Json& args) {
        model.next = {std::move(tool),args,"Check a Session from the origin turn"};
        store.receiveWeb(member,intake,clientId,"Follow up on this work.",message);
        runtime.runConversations();
        check(model.origin == origin,"An explicit reply lost the original Session context");
        return model.result;
    };
    const Json target{{"environmentId","env-1"},{"threadId",thread}};
    check(!call("unrelated","flock_session_events",{{"operation","counts"},{"environmentId","env-1"},
                {"threadId","lineage-unrelated"}}).value("ok",true),"A Bot member could read an unrelated Session");
    auto read = target;
    read["operation"] = "counts";
    check(call("read-child","flock_session_events",read).value("ok",false),
          "The originating Bot could not read its child Session");
    check(model.tasks.at(0).at("session") == target && model.tasks.at(0).contains("launchWarning"),
          "The Bot did not receive the launched Session identity and fallback result");
    auto send = target;
    send["text"] = "Inspect the fixture output";
    const auto sends = effects("t3_thread_send").size();
    check(call("send-child","message_session",send).value("delivery","") == "queued" &&
          effects("t3_thread_send").size() == sends + 1 && effects("t3_thread_send").back().at("threadId") == thread,
          "The originating Bot could not queue a message to its child Session");
    runtime.runConversations();
    check(model.wakes == 0,"The Bot woke before its queued message ran");
    fixture({{"lineageFollowupStatus","completed"}});
    runtime.runConversations();
    runtime.runConversations();
    check(model.wakes == 1 && model.origin == origin && model.result.value("ok",false),
          "The child send did not wake exactly once with the original Session and access to the child");
    auto invite = target;
    invite["botId"] = "triage";
    invite["note"] = "Review the child Session";
    check(call("invite-child","invite_bot",invite).value("depth",0) == 1 &&
          model.invitation.at("sessionOrigin").at("threadId") == thread &&
          model.invitation.at("sessionInvitation").at("inviterBotId") == "release",
          "The Bot could not invite another Bot into the Session it launched");

    participation.stop(*connection,"release","lineage-parent");
    check(!call("stopped","message_session",send).value("ok",true) && effects("t3_thread_send").size() == sends + 1,
          "A reply after stopping origin participation could still send into its child");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "expected fixture URL and credential directory");
        const std::string origin = argv[1];
        const auto endpoint = origin + "/mcp";
        const std::filesystem::path home = argv[2];
        auto fixture = [&](Json configuration) {
            check(http(origin + "/fixture", "POST", {{"Content-Type", "application/json"}},
                       configuration.dump()).status == 200, "fixture rejected configuration");
        };
        auto effects = [&](std::string_view tool) {
            const auto response = http(origin + "/fixture/effects", "GET");
            check(response.status == 200, "could not read fixture effects");
            return Json::parse(response.body).value(std::string(tool), Json::array());
        };
        auto signIn = [&](const std::string& environment) {
            return loginT3(home, environment, endpoint, [&](const std::string& authorization) {
                fixture({{"authorization", authorization}});
            });
        };
        for (const auto& [stage, code] : std::map<std::string, std::string>{
                 {"discovery", "invalid_resource"}, {"registration", "invalid_client_metadata"}, {"token", "invalid_grant"}}) {
            fixture({{"oauthFailure", stage}});
            bool diagnosed = false;
            try { signIn("env-1"); }
            catch (const std::runtime_error& error) {
                diagnosed = std::string(error.what()).find(stage + " failed (HTTP 400): " + code) != std::string::npos;
            }
            check(diagnosed, "OAuth failure identifies stage, HTTP status and error");
        }
        fixture({{"oauthFailure", false}});
        check(signIn("env-1"), "login reports Operate approval");
        auto ownerOnly = [](const std::filesystem::path& path) {
            struct stat st{};
            return stat(path.c_str(), &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
        };
        check(ownerOnly(home), "credential directory is owner-only");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(home))
            check(ownerOnly(entry.path()), "stored credentials are owner-only");
        auto adapter = makeT3Mcp(home);
        Command launch;
        launch.actionId = "flock-action-0123";
        launch.tool = "delegate";
        launch.environmentId = "env-1";
        launch.projectId = "project";
        launch.instruction = "  preserve instruction whitespace and content  ";
        launch.authGeneration = adapter->authGeneration(launch.environmentId, launch.config);
        check(!launch.authGeneration.empty() && !adapter->preflight(launch), "valid credential preflight");
        for (const auto& [mode, detail] : std::map<std::string, std::string>{
                 {"rpc", "T3 JSON-RPC error -32602: Invalid params"}, {"tool", "T3 rejected the operation"}}) {
            fixture({{"projectError", mode}});
            auto error = adapter->preflight(launch);
            check(error && error->kind == Delivery::Kind::Unknown && error->detail == detail,
                  "JSON-RPC and plain-text MCP tool failures remain distinguishable");
        }
        fixture({{"projectError", false}});
        auto checkPreflight = [&](const Command& command, Delivery::Kind kind, std::string_view reason) {
            const auto failure = adapter->preflight(command);
            check(failure && failure->kind == kind, reason);
        };
        auto restarted = makeT3Mcp(home);
        check(restarted->authGeneration("env-1", launch.config) == launch.authGeneration, "persistent OAuth generation");
        auto other = launch;
        other.environmentId = "env-2";
        checkPreflight(other, Delivery::Kind::Auth, "another environment cannot use credential");
        other = launch;
        other.config["session"]["model"] = "missing provider";
        checkPreflight(other, Delivery::Kind::Settings, "partial model selection rejected before send");
        fixture({{"delayRunId", true}});
        auto delivery = adapter->send(launch);
        check(delivery.kind == Delivery::Kind::Unknown && delivery.run.threadId == "thread", "delayed Run ID remains unconfirmed");
        delivery = restarted->reconcile(launch);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "root", "recovered exact launch after restart");
        const auto run = delivery.run;
        other = launch;
        other.launchReceiptThreadId = "thread";
        const auto launchLists = effects("t3_thread_list").size();
        fixture({{"duplicate", true}});
        check(restarted->reconcile(other).kind == Delivery::Kind::Accepted &&
              effects("t3_thread_list").size() == launchLists,
              "a launch receipt scopes reconciliation to its Thread even when other Threads match");
        other.candidate.threadId = "duplicate";
        check(restarted->reconcile(other).kind == Delivery::Kind::Unknown &&
              effects("t3_thread_list").size() == launchLists,
              "a manual hint cannot override the Thread in the launch receipt");
        fixture({{"duplicate", false}});
        other = launch;
        other.instruction += "x";
        check(adapter->reconcile(other).kind == Delivery::Kind::Unknown, "marker requires identical instruction");
        other = launch;
        other.candidate = {"thread", "wrong", {}};
        check(adapter->reconcile(other).kind == Delivery::Kind::Unknown, "manual candidate does not override identity");
        fixture({{"duplicate", true}});
        check(adapter->reconcile(launch).kind == Delivery::Kind::Unknown, "multiple matching threads are ambiguous");
        fixture({{"duplicate", false}});
        const auto launches = effects("t3_thread_launch");
        check(launches.size() == 1, "launch reconciliation creates no additional T3 threads");
        const auto& args = launches.at(0);
        check(args.at("clientRequestId") == launch.actionId &&
              args.at("message") == launch.instruction + "\n\n[Flock action ID: " + launch.actionId + "]",
              "T3 receives the original launch instruction followed by one canonical marker");
        check(!args.contains("modelSelection") && !args.contains("runtimeMode") && !args.contains("delegatedFrom") &&
              !args.contains("workspaceStrategy") && !args.contains("interactionMode"),
              "T3 uses its defaults when Session settings are omitted");

        fixture({{"scenario", "question"}});
        auto observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Question && observation.question.requestId == "request",
              "owned question is exposed");
        const auto& field = observation.question.fields.at(0);
        check(field.at("id") == "color" && field.at("question") == "Which colors?" && field.at("multiple") == true &&
              field.at("freeform") == false, "question retains its prompt and input constraints");
        const auto& options = field.at("options");
        check(options.size() == 2 && options.at(0).at("label") == "Green" && options.at(0).at("value") == "green" &&
              options.at(1).at("label") == "　blue　" && options.at(1).at("value") == "blue",
              "question retains the display labels and answer values for both options");
        Command answer = launch;
        answer.tool = "answer_session_question";
        answer.target = run;
        answer.questionId = "request";
        answer.answers = {{"color", "missing option"}, {"message", ""}, {"tag", ""}};
        check(adapter->send(answer).kind == Delivery::Kind::NotAccepted, "invalid option is rejected");
        answer.answers["color"] = Json::array({"green", "blue"});
        fixture({{"scenario", "ambiguous"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput &&
              adapter->send(answer).kind == Delivery::Kind::NotAccepted, "identical content in another Run blocks auto-answer");
        fixture({{"scenario", "approval"}});
        check(adapter->observe("env-1", launch.config, run).kind == Observation::Kind::ExternalInput, "approvals stay in T3");
        fixture({{"scenario", "question"}});
        check(adapter->reconcile(answer).kind == Delivery::Kind::Unknown, "reconciliation reports the pending question");
        check(effects("t3_pending_request").empty(),
              "T3 receives no reply for an invalid option, ambiguous ownership or unknown delivery");
        check(adapter->send(answer).kind == Delivery::Kind::Accepted &&
              restarted->reconcile(answer).kind == Delivery::Kind::QuestionGone &&
              restarted->send(answer).kind == Delivery::Kind::QuestionGone, "answer uses original request once");
        const auto replies = effects("t3_pending_request");
        check(replies.size() == 1, "T3 receives exactly one reply across restart and retry");
        check(replies.at(0).at("threadId") == "thread" && replies.at(0).at("requestId") == "request" &&
              replies.at(0).at("answers") == Json{{"color", Json::array({"green", "blue"})},
                  {"note", ""}, {"message", ""}, {"tag", ""}},
              "T3 lost a selection, omitted optional text, cleared editor text or the selected empty tag");
        fixture({{"scenario", "race"}});
        check(adapter->send(answer).kind == Delivery::Kind::QuestionGone, "user answer during dispatch restores monitoring");

        const auto submittedAnswers = effects("t3_pending_request").size();
        fixture({{"scenario", "question"}, {"failAnswerRecheck", true}});
        delivery = adapter->send(answer);
        check(delivery.definitelyUnsent && effects("t3_pending_request").size() == submittedAnswers,
              "failed question recheck was recorded as an ambiguous answer or sent a response");
        fixture({{"failAnswerRecheck", false}, {"dropAnswerReply", true}});
        delivery = adapter->send(answer);
        check(delivery.kind == Delivery::Kind::Unknown && !delivery.definitelyUnsent &&
              effects("t3_pending_request").size() == submittedAnswers + 1 &&
              restarted->reconcile(answer).kind == Delivery::Kind::QuestionGone,
              "a lost response was treated as definitely unsent or retried");
        fixture({{"dropAnswerReply", false}});

        observation = adapter->observe("env-1", Json::array(), run);
        check(observation.kind == Observation::Kind::Unavailable && observation.status == "settings",
              "runtime recognizes observation settings failures");
        fixture({{"scenario", "settling"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Settling, "wait timeout preserves outstanding work");
        fixture({{"scenario", "settling-input"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::ExternalInput &&
              observation.question.requestId.empty() && observation.question.fields.empty(),
              "pending input during delegated settlement is handed off to T3");
        const auto checkResult = [&](const char* scenario, bool ownsLater) {
            fixture({{"scenario", scenario}});
            const auto observed = adapter->observe("env-1", launch.config, run);
            check(observed.kind == Observation::Kind::Finished && observed.resultRunId == (ownsLater ? "later" : "root") &&
                  observed.report == (ownsLater ? "later report" : "root report"),
                  std::string("Incorrect result attribution in ") + scenario);
        };
        checkResult("notification-result", true);
        checkResult("shortened-result", true);
        checkResult("nested-result", true);
        checkResult("mixed-result", false);
        checkResult("server-input-result", false);
        checkResult("ambiguous-title-result", true);
        checkResult("failed-result", true);
        checkResult("stopped-result", true);
        checkResult("unlabeled-result", true);
        checkResult("batch-result", true);
        checkResult("partial-batch-result", true);
        checkResult("batch-overcount-result", false);
        checkResult("invalid-batch-count-result", false);
        checkResult("zero-batch-count-result", false);
        checkResult("malformed-batch-count-result", false);
        checkResult("late-settlement-result", false);
        checkResult("foreign-child-result", false);
        checkResult("native-subagent-result", false);
        checkResult("reused-result", false);

        fixture({{"scenario", "paged-report"}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Finished && !observation.truncated &&
              observation.itemId == "item-1" && observation.report == std::string(7000, 'x') + "end of report",
              "Report reads older pages for the assigned Run and joins text continuations");
        fixture({{"changeReportDuringRead", true}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Finished && observation.report.empty() &&
              observation.evidence == RunEvidence::Availability::ReportUnavailable,
              "Report rejects changes during text continuation without erasing settlement");
        fixture({{"changeReportDuringRead", false}});
        using Availability = RunEvidence::Availability;
        for (const auto* scenario : {"settled", "notification-result"}) {
            const bool later = std::string_view(scenario) == "notification-result";
            fixture({{"scenario",scenario},{"reportReadFailure","messages"}});
            const auto observed = adapter->observe("env-1",launch.config,run);
            check(observed.kind == Observation::Kind::Finished && observed.status == "completed" &&
                  observed.resultRunId == (later ? "later" : "root") && observed.report.empty() &&
                  observed.evidence == Availability::ReportUnavailable && !observed.detail.empty(),
                  "unreadable report erased settlement or its proven source");
            fixture({{"reportReadFailure",nullptr},{"waitFailure",true}});
            const auto evidence = restarted->readRunEvidence("env-1",launch.config,
                {run.threadId,observed.resultRunId,run.link},observed.status);
            check(evidence.availability == Availability::Available &&
                  evidence.report == (later ? "later report" : "root report"),
                  "report recovery depended on another wait or read the wrong Run");
            fixture({{"waitFailure",false}});
        }
        fixture({{"scenario","settled"},{"terminalStatus","failed"},{"reportReadFailure","activity"}});
        observation = adapter->observe("env-1",launch.config,run);
        check(observation.kind == Observation::Kind::Finished && observation.status == "failed" &&
              observation.resultRunId == "root" && observation.report == "root report" &&
              observation.evidence == Availability::DiagnosticUnavailable && !observation.detail.empty(),
              "unreadable error history erased a proven failure or its written report");
        fixture({{"scenario","notification-result"},{"terminalStatus","completed"},
                 {"reportReadFailure",nullptr},{"ownershipReadFailure",true}});
        observation = adapter->observe("env-1",launch.config,run);
        check(observation.kind == Observation::Kind::Unavailable && observation.resultRunId.empty(),
              "unreadable ownership evidence confirmed another Run's result");
        fixture({{"ownershipReadFailure",false}});

        Command follow = launch;
        follow.actionId = "flock-action-01234";
        follow.tool = "follow_up";
        follow.target = run;
        follow.instruction = std::string(700, 'x') + "\n\n[Flock action ID: " + follow.actionId + "]";
        for (const auto* scenario : {"manual-active", "server-input-result"}) {
            fixture({{"scenario", scenario}});
            check(adapter->send(follow).kind == Delivery::Kind::NotAccepted,
                  std::string("Follow-up accepted in ") + scenario);
        }
        check(effects("t3_thread_send").empty(), "T3 received a follow-up for a busy or unrelated Run");
        fixture({{"scenario", "settled"}});
        delivery = adapter->send(follow);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "follow", "follow-up starts a new Run in the original thread");
        delivery = restarted->reconcile(follow);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "follow", "reconciliation recovers the follow-up from paginated history");
        const auto followUps = effects("t3_thread_send");
        check(followUps.size() == 1, "follow-up reconciliation does not send another queued message");
        check(followUps.at(0).at("message") == follow.instruction,
              "T3 receives all of the follow-up instruction");

        Command selected = launch;
        selected.tool = "start_in_session";
        selected.actionId = "flock-selected-idle";
        selected.target = {"human-selected",{},{}};
        selected.instruction = "Add regression tests";
        fixture({{"scenario","selected-session"}});
        check(!adapter->preflight(selected),"an existing Session does not require a Flock Run");
        delivery = adapter->send(selected);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "human-selected" &&
              delivery.run.runId == "selected-run","a previously unrelated idle Session starts the assigned Run");
        check(restarted->reconcile(selected).run.runId == "selected-run",
              "an existing Session's started Run can be recovered from its request");
        fixture({{"selectedRunStatus","cancelled"},{"selectedDelegated",true}});
        const auto draining = adapter->observe("env-1",selected.config,delivery.run);
        check(draining.kind == Observation::Kind::Settling && draining.resultRunId.empty(),
              "cancelling a started Run must keep its delegated work outstanding");
        fixture({{"selectedDelegated",false}});
        const auto drained = restarted->observe("env-1",selected.config,delivery.run);
        check(drained.kind == Observation::Kind::Finished && drained.resultRunId == "selected-run",
              "cancellation settles after the assigned Run's delegated work ends");

        selected.actionId = "flock-selected-busy";
        selected.target = {"flock-selected",{},{}};
        selected.instruction = std::string(1200,'x');
        fixture({{"selectedBusy",true},{"selectedRunStatus",nullptr},{"selectedDropSendReply",true}});
        const auto sendsBeforeQueue = effects("t3_thread_send").size();
        check(adapter->send(selected).kind == Delivery::Kind::Unknown,"fixture lost the queued send response");
        fixture({{"selectedDropSendReply",false}});
        delivery = restarted->reconcile(selected);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "flock-selected" &&
              delivery.run.runId == "selected-run" && effects("t3_thread_send").size() == sendsBeforeQueue + 1,
              "reconciliation recovers the assigned queued Run without resending");
        const auto queued = adapter->observe("env-1",selected.config,delivery.run);
        check(queued.kind == Observation::Kind::Running && queued.status == "queued",
              "an earlier Run's pending question is not attributed to the queued Run");
        check(adapter->interrupt("env-1",selected.config,delivery.run) == InterruptOutcome::Requested &&
              effects("t3_queue") == Json::array({{{"action","cancel"},{"threadId","flock-selected"},
                                                   {"queuedRunId","selected-run"}}}) &&
              effects("t3_thread_interrupt").empty(),"stop removes only the assigned queued Run");
        for (const auto& [conditions,error] : std::vector<std::pair<Json,std::string>>{
            {{{"selectedRestartPending",true}}, "a pending restart must retain the assigned Run"},
            {{{"selectedOmitStart",true}}, "missing start history cannot prove a queue cancellation"},
            {{{"selectedEffectiveRun","continued"}}, "a different effective Run cannot use the assigned Run's queue history"}
        }) {
            fixture(conditions);
            check(restarted->observe("env-1",selected.config,delivery.run).kind != Observation::Kind::Finished,error);
            fixture({{"selectedRestartPending",false},{"selectedOmitStart",false},{"selectedEffectiveRun",nullptr}});
        }
        const auto cancellation = restarted->observe("env-1",selected.config,delivery.run);
        check(cancellation.kind == Observation::Kind::Finished &&
              cancellation.status == "cancelled" && cancellation.resultRunId == "selected-run",
              "other active work does not hold cancellation of the assigned Run");

        fixture({{"selectedBadReceipt",true}});
        check(adapter->send(selected).kind == Delivery::Kind::Unknown,
              "a receipt pointing to the previous active Run cannot claim that Run");
        const auto selectedSends = effects("t3_thread_send").size();
        fixture({{"selectedBadReceipt",false},{"selectedArchived",true}});
        check(adapter->send(selected).kind == Delivery::Kind::Settings,
              "an archived Session rejected the request before sending");
        fixture({{"selectedArchived",false},{"selectedProject","another-project"}});
        check(adapter->send(selected).kind == Delivery::Kind::Settings &&
              effects("t3_thread_send").size() == selectedSends,
              "a changed destination project rejected the request before sending");
        fixture({{"scenario","settled"}});

        ReadRequest read;
        read.environmentId = "env-1";
        read.threadId = "thread";
        read.runId = "root";
        read.itemId = "unicode";
        std::string expectedText = "界";
        for (int i = 0; i < 3000; ++i) expectedText += "🙂";
        expectedText += "犬";
        for (int i = 0; i < 3000; ++i) expectedText += "🐦";
        expectedText += "\n夕焼け";
        std::string receivedText;
        Excerpt chunk;
        do {
            chunk = adapter->read(read);
            check(!chunk.text.empty() && chunk.text.size() <= 8192, "each Unicode excerpt fits the byte limit");
            check(chunk.threadId == "thread" && chunk.runId == "root" && chunk.itemId == "unicode",
                  "continuation retains the excerpt's thread, Run and item");
            receivedText += chunk.text;
            check(receivedText.size() <= expectedText.size(), "continuation does not repeat text");
            check(!chunk.more || chunk.nextOffset > read.offset, "a continued excerpt advances its cursor");
            read.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedText == expectedText, "continued excerpts preserve the complete Unicode text");
        ReadRequest translation = read;
        translation.itemId = "translation";
        translation.offset = 0;
        std::string expectedTranslation;
        for (int i = 0; i < 2800; ++i) expectedTranslation += "—";
        std::string receivedTranslation;
        do {
            chunk = adapter->read(translation);
            check(chunk.itemId == "translation" && !chunk.text.empty() && chunk.text.size() <= 8192,
                  "translated fixture excerpts retain their item and byte boundary");
            receivedTranslation += chunk.text;
            translation.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedTranslation == expectedTranslation,"English fixture translation was not preserved");
        ReadRequest denseBmp = read;
        denseBmp.runId = "root";
        denseBmp.itemId = "dense-bmp";
        denseBmp.offset = 0;
        std::string expectedBmp;
        for (int i = 0; i < 7000; ++i) expectedBmp += "界";
        std::string receivedBmp;
        do {
            chunk = adapter->read(denseBmp);
            check(chunk.itemId == "dense-bmp" && !chunk.text.empty() && chunk.text.size() <= 8192,
                  "dense BMP excerpts stay within the byte limit");
            receivedBmp += chunk.text;
            check(receivedBmp.size() <= expectedBmp.size(), "dense BMP continuation does not repeat text");
            check(!chunk.more || chunk.nextOffset > denseBmp.offset, "dense BMP continuation advances its cursor");
            denseBmp.offset = chunk.nextOffset;
        } while (chunk.more);
        check(receivedBmp == expectedBmp, "dense BMP excerpts preserve the complete text");
        read.offset = 0;
        read.runId = "wrong";
        bool rejected = false;
        try { adapter->read(read); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "foreign item cannot be read");
        check(adapter->interrupt("env-1", launch.config, run) == InterruptOutcome::AlreadyTerminal &&
              effects("t3_thread_interrupt").empty(), "T3 receives no interrupt for an already settled Run");
        fixture({{"scenario", "running"}});
        check(adapter->interrupt("env-1", launch.config, run) == InterruptOutcome::Requested &&
              effects("t3_thread_interrupt") == Json::array({{{"threadId", "thread"}, {"runId", "root"}}}),
              "T3 receives exactly one interrupt for the assigned Run");
        fixture({{"interruptReply", "no_active_run"}});
        check(adapter->interrupt("env-1", launch.config, run) == InterruptOutcome::NoActiveRun,
              "a Run becoming inactive before the interrupt was reported as acknowledged");
        fixture({{"interruptReply", "completed"}});
        check(adapter->interrupt("env-1", launch.config, run) == InterruptOutcome::AlreadyTerminal,
              "a Run ending before the interrupt was reported as acknowledged");
        fixture({{"interruptReply", false}});

        signIn("env-1");
        const auto reauthorized = adapter->authGeneration("env-1", launch.config);
        check(reauthorized != launch.authGeneration, "reauthorization changes generation");
        other = launch;
        other.authGeneration = reauthorized;
        check(!adapter->preflight(other), "reauthorization retains permission to operate");
        const auto staleAuth = adapter->send(launch);
        check(staleAuth.kind == Delivery::Kind::Auth && staleAuth.definitelyUnsent,
              "stale generation blocks commands before send");
        check(effects("t3_thread_launch").size() == 1, "stale authorization never launches another T3 thread");

        sessionMessageWake(*restarted,home.parent_path()/"session-wake.db",fixture,effects);
        fixture({{"scope", "orchestration:read"}});
        check(!signIn("env-1"), "login reports read-only approval");
        other.authGeneration = adapter->authGeneration("env-1", launch.config);
        checkPreflight(other, Delivery::Kind::Auth, "read-only OAuth cannot operate");
        check(adapter->send(other).kind == Delivery::Kind::Auth &&
              effects("t3_thread_launch").size() == 1, "read-only authorization never launches a T3 thread");
        delivery = adapter->reconcile(launch);
        check(delivery.kind == Delivery::Kind::Accepted && delivery.run.threadId == "thread" &&
              delivery.run.runId == "root", "reauthorization with read scope recovers the original Run");
        const auto readOnly = other.authGeneration;
        const auto [allProjects,allProjectsNext] = restarted->projectSessions("env-1",{},0,{});
        check(allProjects == std::vector<std::string>{"first","second"} && !allProjectsNext,
              "a connected Environment lists Sessions across readable projects");
        fixture({{"scenario","session-read"}});
        deletedSessionSearch(*restarted,home.parent_path()/"session-search.db",fixture);
        boundedSessionSearch(*restarted,home.parent_path()/"bounded-search.db",fixture,effects);
        sessionSearchDeadline(*restarted,home.parent_path()/"search-deadline.db",fixture);
        fixture({{"scenario","catalog-timeout"},{"delayCatalog",true}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        const auto [partial,offset] = restarted->projectSessions("env-1","project",0,deadline);
        check(partial.size() == 50 && offset == 50,"Catalog timeout discarded a completed page");
        fixture({{"delayCatalog",false}});
        const auto [remaining,nextOffset] = restarted->projectSessions("env-1","project",*offset,{});
        check(remaining == std::vector<std::string>{"catalog-50"} && !nextOffset,
              "Catalog continuation did not resume the timed-out page");
        fixture({{"scenario","session-read"}});
        const SessionTarget session{"env-1","manual"};
        const auto metadata = restarted->sessionMetadata(session,true);
        check(metadata && metadata->project == "Release Pipeline" && metadata->title == "Staging investigation" &&
              metadata->status == "failed" &&
              std::ranges::any_of(metadata->messages,[](const SessionPreview& message) {
                  return message.text == "staging health check failed";
              }),"Session metadata or local preview lost");
        check(std::ranges::none_of(metadata->messages,[](const SessionPreview& message) {
            return message.text == "private history" || message.text.size() > 720;
        }),"Session preview exceeded its bounds or contained inherited text");
        for (const auto* code : {"invalid_request","project_not_found"}) {
            fixture({{"projectReadFailure",{{"code",code},{"message","The project was not found."}}}});
            const auto missingProject = restarted->sessionMetadata(session,false);
            check(missingProject && missingProject->project.empty() && missingProject->projectId == "project" &&
                  missingProject->title == metadata->title,"missing project hid readable Session metadata");
        }
        fixture({{"projectReadFailure",nullptr}});
        SessionEventFilter filter{};
        filter.limit = 2;
        const auto firstPage = restarted->sessionEvents(session,filter);
        check(firstPage && firstPage->items.size() == 2 && firstPage->nextPosition &&
              firstPage->items[0].kind == "user_message" && firstPage->items[1].kind == "command_execution",
              "Session history did not return an ordered first page");
        filter.afterPosition = firstPage->nextPosition;
        const auto secondPage = restarted->sessionEvents(session,filter);
        check(secondPage && secondPage->items.size() == 2 && secondPage->items[0].id == "item-2",
              "Session history did not resume after the previous page");
        filter = {};
        filter.matchText = [](const SessionEvent& event) { return session_read::contains(event.text,"HEALTH"); };
        filter.kind = "error";
        filter.fromTime = "2026-01-01T00:00:00Z";
        filter.toTime = "2026-01-01T00:00:00Z";
        const auto matched = restarted->sessionEvents(session,filter);
        check(matched && matched->items.size() == 1 && matched->items[0].id == "item-2",
              "Session event filtering did not find the failed health check");
        const auto command = restarted->sessionEvent(session,"item-1");
        const auto error = restarted->sessionEvent(session,"item-2");
        const auto longEvent = restarted->sessionEvent(session,"item-7");
        check(command && command->text == "$ ctest --output-on-failure\nTest 7 failed" &&
              command->attachments.size() == 2 && error && error->text == "staging health check failed" &&
              longEvent && longEvent->textTruncated && longEvent->text.size() <= 8192,
              "Session event details, attachment references or truncation lost");
        check(!restarted->sessionEvent(session,"item-6"),"inherited item exposed another Session");
        const auto totals = restarted->sessionCounts(session);
        check(totals && totals->messages == 5 && totals->runs == 4,"counts were derived from a partial page");
        fixture({{"longHistory",true}});
        filter = {};
        filter.kind = "checkpoint";
        filter.limit = 1;
        for (Id expected : {11,23}) {
            const auto partial = restarted->sessionEvents(session,filter);
            check(partial && partial->items.empty() && partial->nextPosition == expected,
                  "selective event search did not return a progressing cursor at its scan bound");
            filter.afterPosition = partial->nextPosition;
        }
        const auto resumed = restarted->sessionEvents(session,filter);
        check(resumed && resumed->items.size() == 1 && resumed->items.front().id == "item-25" && !resumed->nextPosition,
              "resuming a bounded scan skipped a local event");
        fixture({{"longHistory",false}});
        const auto attachment = restarted->sessionAttachment(session,"item-1","test-log");
        const auto reference = restarted->sessionAttachment(session,"item-1","reference-only");
        check(attachment && attachment->content && attachment->content->size() > 8192 &&
              attachment->content->starts_with("Test 7 failed") && reference && !reference->content,
              "attachment did not preserve content bounds or reference-only evidence");
        fixture({{"denyAttachment",true}});
        check(!restarted->sessionAttachment(session,"item-1","test-log") &&
              !restarted->sessionAttachment(session,"item-1","missing"),"denied attachment was distinguishable from missing");
        fixture({{"denyRead",true}});
        const SessionTarget missing{"env-1","missing"};
        filter.afterPosition = firstPage->nextPosition;
        check(!restarted->sessionMetadata(session,false) && !restarted->sessionMetadata(missing,false) &&
              !restarted->sessionEvents(session,filter) && !restarted->sessionEvent(session,"item-1") &&
              !restarted->sessionCounts(session) && !restarted->sessionAttachment(session,"item-1","test-log"),
              "history references bypassed a T3 read denial");
        fixture({{"scenario","settled"}});
        fixture({{"revoked", true}});
        observation = adapter->observe("env-1", launch.config, run);
        check(observation.kind == Observation::Kind::Unavailable && observation.status == "auth",
              "server-side OAuth revocation is surfaced");
        rejected = false;
        try { signIn("env-2"); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "OAuth login rejects a different canonical environment ID");
        other = launch;
        other.environmentId = "env-2";
        checkPreflight(other, Delivery::Kind::Auth, "mismatched login leaves no usable credential");
        check(adapter->authGeneration("env-1", launch.config) == readOnly,
              "mismatched login preserves the credential for the canonical environment");

        fixture({{"scope","orchestration:read orchestration:operate"},{"denyRead",false},{"lineageFixture",true},
                 {"lineageSessions",Json::object()}});
        check(signIn("env-1"),"Could not restore the Operate credential for lineage tests");
        lineageFallback(*adapter,launch,fixture,effects,[&] {
            const auto response = http(origin + "/fixture/lineage-sessions","GET");
            check(response.status == 200,"Could not inspect the fixture's created Sessions");
            return Json::parse(response.body);
        });
        sessionOriginLaunch(*adapter,home.parent_path()/"session-origin-launch.db",fixture,effects);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
