#include "http_fixture.hpp"
#include "model.hpp"

#include "flock/bot_access.hpp"
#include "flock/command.hpp"
#include "flock/policy_rules.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task_list.hpp"
#include "flock/tasks.hpp"

#include <algorithm>
#include <utility>

namespace flockv0::http_test {
namespace {
constexpr auto explicitRequest = "Repair the null check in billing/api.ts";
constexpr auto dismissedRequest = "Cover nil refunds in billing/refund_test.ts";
constexpr auto editedRequest = "Cover nil invoices and legacy IDs in billing/invoice_test.ts";
constexpr auto view = "/api/view?bot=release";
constexpr auto history = "/api/conversation?bot=release";
constexpr auto startEndpoint = "/api/suggested-tasks/start";
constexpr auto dismissEndpoint = "/api/suggested-tasks/dismiss";
const Person requester{"reviewer", false};

Json eventWith(const Json& page, const auto& matches) {
    const auto& events = page.at("events");
    const auto found = std::ranges::find_if(events, matches);
    check(found != events.end(), "Expected conversation entry was absent: " + page.dump());
    return *found;
}

Json replyTo(const Json& page, Id message) {
    return eventWith(page, [&](const Json& event) {
        return event.at("kind") == "conversation_reply" && event.at("data").at("replyToMessageId") == message;
    });
}

const Json& rows(const Json& card) {
    return card.at("data").at("suggestedTasks");
}

Json suggestion(std::string_view request, std::string_view reason) {
    return {{"request", request}, {"reason", reason}};
}

ModelTurn toolCall(const ModelInput& input, std::string_view name, const Json& arguments) {
    return app::responseDecision(Json::array({{{"type", "function_call"},
                                               {"namespace", "flock"},
                                               {"status", "completed"},
                                               {"name", name},
                                               {"arguments", arguments.dump()}}}),
                                 app::responseRequest(input));
}

struct SuggestionModel : Model {
    unsigned explicitTaskCalls = 0;

    ModelTurn decide(const ModelInput& input) override {
        const auto& event = input.event;
        if (event.at("kind") != "conversation_turn") {
            if (input.task.at("request") == explicitRequest) ++explicitTaskCalls;
            if (event.at("kind") == "intake") {
                Json args = {{"instruction", input.task.at("request")}, {"reason", "Carry out the requested work"}};
                if (input.needsDoneWhen) args["doneWhen"] = "Verify the changed code";
                return toolCall(input, "delegate", args);
            }
            check(event.at("kind") == "result", "The finished Run could not complete");
            auto unavailable = suggestion("Cover nil imported IDs", "Import still accepts null IDs");
            unavailable["environment"] = "unavailable-environment";
            return toolCall(
                input, "complete",
                {{"report", "The null check is in place."},
                 {"suggestedTasks", Json::array({suggestion("Audit the parser in billing/import.ts", "Import still accepts null IDs"),
                                                 suggestion(dismissedRequest, "Refunds also use the parser"), std::move(unavailable)})},
                 {"reason", "The Run verified the null check"}});
        }

        const auto& steps = event.at("turn");
        const auto& text = event.at("text");
        if (text == explicitRequest) {
            if (steps.empty())
                return toolCall(input, "start_task", {{"request", explicitRequest}, {"doneWhen", "The billing null-check test passes"}});
            return toolCall(input, "reply", {{"text", "Started the billing fix."}});
        }

        if (steps.empty()) {
            if (text == "What other work did you find?") {
                auto edit = suggestion("Cover nil invoices in billing/invoice_test.ts", "Invoices also lack a nil-ID fixture");
                edit["doneWhen"] = "The invoice tests cover the legacy input";
                edit["environment"] = "mini";
                auto retries = suggestion("Check charge retries in billing/charge.ts", "Retry still passes a nullable ID");
                retries["environments"] = Json::array({"billing-charges", "backup-charges"});
                auto import = suggestion("Make invoice import deterministic", "The import fixture reuses the database");
                import["environment"] = "billing-import";
                return toolCall(input, "suggest_tasks",
                                {{"items", Json::array({std::move(edit), suggestion(dismissedRequest, "Refunds use the same parser"),
                                                        std::move(retries), std::move(import)})}});
            }
            auto differentDestination = suggestion(explicitRequest, "Run the same fix in the backup checkout");
            differentDestination["environments"] = Json::array({"mini"});
            return toolCall(
                input, "suggest_tasks",
                {{"items", Json::array({suggestion("  repair the null check in billing/api.ts  ", "Matches the active request"),
                                        suggestion(dismissedRequest, "The duplicate should stay dismissed"), differentDestination})}});
        }
        const auto& result = steps.back().at("result");
        const int expected = text == "What other work did you find?" ? 4 : 1;
        check(result.at("ok") == true && result.at("count") == expected, "Unexpected suggest_tasks result: " + result.dump());
        return toolCall(input, "reply", {{"text", "Here is the work I found."}, {"suggestions", {"Tell me more"}}});
    }
};

struct SuggestionT3 : T3 {
    unsigned launches = 0;
    std::string finishedRun;

    std::string authGeneration(const std::string&, const Json&) override { return "suggestion-fixture"; }
    Delivery reconcile(const Command&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
    Delivery send(const Command& command) override {
        check(command.tool == CommandTool::delegate, "Unexpected T3 command");
        const auto suffix = std::to_string(++launches);
        return {Delivery::Kind::Accepted, {"thread-" + suffix, "run-" + suffix, {}}, {}};
    }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        if (run.runId != finishedRun) return {};
        return {
            .kind = Observation::Kind::Finished, .status = "completed", .report = "Verified the null check", .itemId = "billing-report"};
    }
};

WebCallbacks suggestionCallbacks() {
    auto web = callbacks();
    web.authenticate = [](std::string_view auth) { return auth == "Bearer reviewer" ? requester : Person{}; };
    web.resolveIdentity = [](std::string_view id) -> std::optional<bool> {
        if (id == requester.id) return requester.adminRoot;
        return std::nullopt;
    };
    const auto intake = web.intake;
    web.intake = [intake](std::string_view bot, std::string_view request) {
        auto result = intake(bot, request);
        result.config["conversationEnvironments"] = {{"mini", {{"environmentId", "mini-env"}, {"projectId", "mini-project"}}},
                                                     {"billing-charges", {{"environmentId", "local-env"}, {"projectId", "charges"}}},
                                                     {"backup-charges", {{"environmentId", "mini-env"}, {"projectId", "backup-charges"}}},
                                                     {"billing-import", {{"environmentId", "local-env"}, {"projectId", "import"}}}};
        result.config["modelProvider"] = "chatgpt";
        result.config["model"] = "fixture-model";
        return result;
    };
    return web;
}

Id send(Browser& browser, Runtime& runtime, std::string_view text) {
    const auto receipt = browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", text}, {"text", text}}, 201);
    runtime.runConversations();
    return receipt.at("messageId");
}
}

void suggestedTaskEndpoints(const std::filesystem::path& path) {
    Id source = 0;
    Json saved;
    {
        Store store(path);
        BotAccess{store}.setMember(localOwner, "release", requester.id, true);
        SuggestionModel model;
        SuggestionT3 t3;
        const auto web = suggestionCallbacks();
        Runtime runtime(store, model, t3, [](auto) { return std::string{}; }, RuntimeOptions{.intake = web.intake});
        const auto server = startWeb(store, 0, web);
        Browser browser(*server);
        browser.client.set_default_headers({{"Authorization", "Bearer reviewer"}});

        const auto explicitMessage = send(browser, runtime, explicitRequest);
        tick(runtime, 4);
        const auto tasks = TaskList(store).tasks("release");
        check(tasks.size() == 1 && t3.launches == 1 && tasks.front().task.request == explicitRequest &&
                  tasks.front().task.sourceMessageId == explicitMessage &&
                  !replyTo(browser.get(view), explicitMessage).at("data").contains("suggestedTasks"),
              "The explicitly requested work did not start directly");
        const auto explicitTask = tasks.front().task.id;

        PolicyRules rules{store};
        rules.setMode(localOwner, "supervised");
        const auto suggestionMessage = send(browser, runtime, "What other work did you find?");
        const auto page = browser.get(view);
        const auto reply = replyTo(page, suggestionMessage);
        source = reply.at("id");
        const auto proposed = rows(reply);
        check(TaskList(store).tasks("release").size() == 1 && browser.get("/api/inbox").at("items").empty() &&
                  reply.at("data").at("suggestions") == Json::array({"Tell me more"}) && proposed.size() == 4 &&
                  std::ranges::all_of(proposed, [](const Json& item) { return item.at("status") == "suggested"; }),
              "Follow-up tasks became real work, replaced tap replies, or entered Needs you");
        const auto first = proposed.at(0).at("id");
        const auto dismissed = proposed.at(1).at("id");
        check(proposed.at(0).at("destination") == "mini" && proposed.at(0).at("reason") == "Invoices also lack a nil-ID fixture",
              "The card lost the selected destination or reason");

        const Json edited = {{"botId", "release"}, {"sourceMessageId", source}, {"items", {{{"id", first}, {"request", editedRequest}}}}};
        browser.post(dismissEndpoint, {{"botId", "triage"}, {"id", first}}, 403);
        rules.changeNeverRule(localOwner, Action::environment_launch, "mini-env", true);
        browser.post(startEndpoint, edited, 403);
        check(TaskList(store).tasks("release").size() == 1 && rows(replyTo(browser.get(view), suggestionMessage)) == proposed,
              "A denied mutation changed the card or created a task");
        rules.changeNeverRule(localOwner, Action::environment_launch, "mini-env", false);
        browser.post(dismissEndpoint, {{"botId", "release"}, {"id", dismissed}});
        const auto launched = browser.post(startEndpoint, edited);
        check(browser.post(startEndpoint, edited) == launched, "Retrying Start changed its launch receipt");
        const auto live = browser.get(std::string(view) + "&changesAfter=" + page.at("historyCursor").dump());
        check(live.at("changedMessageIds") == Json::array({source}), "Suggestion mutations did not invalidate their conversation entry");
        const auto changed = rows(replyTo(live, suggestionMessage));
        check(changed.at(0).at("status") == "started" && changed.at(0).at("request") == editedRequest &&
                  changed.at(1).at("status") == "dismissed",
              "The live card did not keep its edited request and dismissal");

        browser.post(startEndpoint, {{"botId", "release"},
                                     {"sourceMessageId", source},
                                     {"items", {{{"id", proposed.at(2).at("id")}}, {{"id", proposed.at(3).at("id")}}}}});
        tick(runtime, 8);
        saved = rows(replyTo(browser.get(history), suggestionMessage));
        check(saved.at(2).at("status") == "started" && saved.at(3).at("status") == "started" &&
                  saved.at(2).at("taskId") != saved.at(3).at("taskId") && TaskList(store).tasks("release").size() == 4,
              "Start all did not start each remaining item once");
        check(t3.launches == 5,
              "Expected five T3 launches, got " + std::to_string(t3.launches) + ": " + taskJson(TaskList(store).tasks("release")).dump());
        const auto started = Tasks(store).task(saved.at(0).at("taskId")).task;
        check(started.requester == requester.id && started.sourceMessageId == source && started.request == editedRequest &&
                  started.doneWhen == "The invoice tests cover the legacy input" && started.environmentId == "mini-env" &&
                  started.projectId == "mini-project" && started.state == TaskState::running &&
                  browser.get("/api/inbox").at("items").empty(),
              "The selected launch did not retain its requester, source, criteria and destination, or needed a second approval");
        const auto linked =
            eventWith(browser.get(history), [&](const Json& event) { return event.at("id") == saved.at(0).at("taskEventId"); });
        check(linked.at("taskId") == started.id, "The started row did not link to its task");

        const auto dedupMessage = send(browser, runtime, "Did you find anything else?");
        const auto unique = rows(replyTo(browser.get(view), dedupMessage));
        check(unique.size() == 1 && unique.front().at("request") == explicitRequest && unique.front().at("destination") == "mini",
              "Deduplication accepted repeated work or ignored the selected destination");

        const auto beforeCompletion = model.explicitTaskCalls;
        t3.finishedRun = "run-1";
        tick(runtime, 5);
        const auto completed = eventWith(
            browser.get(history), [&](const Json& event) { return event.at("kind") == "completed" && event.at("taskId") == explicitTask; });
        check(model.explicitTaskCalls == beforeCompletion + 1 && rows(completed).size() == 1 &&
                  rows(completed).front().at("request") == "Audit the parser in billing/import.ts" &&
                  TaskList(store).tasks("release").size() == 4,
              "Closing the task failed to keep follow-ups as suggestions in the same model call");
        const auto followUp = browser.post(
            startEndpoint,
            {{"botId", "release"}, {"sourceMessageId", completed.at("id")}, {"items", {{{"id", rows(completed).front().at("id")}}}}});
        check(Tasks(store).task(followUp.at("started").front().at("taskId")).task.sourceMessageId == completed.at("id").get<Id>(),
              "Starting a completion follow-up lost the message it came from");
    }

    Store reopened(path);
    const auto server = startWeb(reopened, 0, suggestionCallbacks());
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", "Bearer reviewer"}});
    const auto restored = eventWith(browser.get(history), [&](const Json& event) { return event.at("id") == source; });
    check(rows(restored) == saved, "Started and dismissed suggestions changed after restart");
}
}
