#include "flock/conversation_intake.hpp"
#include "flock/task_history.hpp"
#include "flock/notifications.hpp"
#include "flock/tasks.hpp"
#include "flock/conversations.hpp"
#include "flock/person.hpp"
#include "flock/policy_rules.hpp"
#include "flock/secret_ref.hpp"
#include "flock/secret_requests.hpp"
#include "flock/command.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace flockv0;

namespace {
constexpr std::size_t maxBytes = 192'000, maxExpandedBytes = 65'536;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct SessionService : T3 {
    std::vector<Command> commands;
    std::set<std::string> interrupts;
    std::map<std::string, Observation> observations;
    std::map<std::string, RunRef> runs;
    bool loseReceipt = false;
    std::function<void()> onObserve;

    std::string authGeneration(const std::string&, const Json&) override { return "test"; }
    Delivery send(const Command& command) override {
        if (command.tool == CommandTool::answer_session_question)
            return {Delivery::Kind::NotAccepted, {}, "Answer definitely unsent", true};
        commands.push_back(command);
        auto suffix = std::to_string(commands.size());
        RunRef run{command.tool == CommandTool::delegate ? "thread-" + suffix : command.target.threadId, "run-" + suffix, "link-" + suffix};
        require(runs.emplace(command.actionId, run).second, "duplicate T3 dispatch");
        if (std::exchange(loseReceipt, false)) return {Delivery::Kind::Unknown, {}, {}};
        return {Delivery::Kind::Accepted, run, {}};
    }
    Delivery reconcile(const Command& command) override {
        const auto sent = std::ranges::find(commands, command.actionId, &Command::actionId);
        require(sent != commands.end() && sent->instruction == command.instruction, "reconciliation changed the dispatched instruction");
        return {Delivery::Kind::Accepted, runs.at(command.actionId), {}};
    }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        if (auto callback = std::exchange(onObserve, {})) callback();
        return observations[run.runId];
    }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef& run) override {
        interrupts.insert(run.runId);
        return InterruptOutcome::Requested;
    }
    void finish(std::string run, std::string report, std::string status = "completed") {
        auto& observation = observations[run];
        observation.kind = Observation::Kind::Finished;
        observation.status = std::move(status);
        observation.report = std::move(report);
    }
};

struct Bot : Model {
    Decision action;
    Json history, result;
    std::function<Decision(const ModelInput&)> conversation;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        if (input.event.at("kind") != "conversation_turn") throw std::runtime_error("Unexpected task decision: " + input.event.dump());
        history = input.history;
        if (conversation) return conversation(input);
        if (input.event.contains("sessionWake")) return {"reply", {{"text", "The work finished"}}, "Tell the person"};
        if (!action.tool.empty())
            require(std::ranges::find(input.tools, action.tool) != input.tools.end(), "tool missing from conversation runtime");
        if (input.event.at("turn").empty() && !action.tool.empty()) return action;
        if (!input.event.at("turn").empty()) result = input.event.at("turn").back().at("result");
        return {"reply", {{"text", "Received"}}, "Acknowledge the turn"};
    }
};

struct Fixture {
    std::filesystem::path dir;
    Bot bot;
    SessionService t3;
    int sequence = 0;
    Seconds now = unixTime();
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    explicit Fixture() {
        char name[] = "/tmp/flock-conversation-context-XXXXXX";
        if (!mkdtemp(name)) throw std::runtime_error("Cannot create test directory");
        dir = name;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(dir);
    }
    void restart() {
        runtime.reset();
        store.reset();
        store = std::make_unique<Store>(dir / "state.db");
        RuntimeOptions options;
        options.clock = [this] { return now; };
        runtime = std::make_unique<Runtime>(*store, bot, t3, [](auto) { return std::string{}; }, options);
    }
    Id turn(std::string text, std::string tool = {}, Json args = Json::object()) {
        bot.action = {tool, args, "Execute the requested operation"};
        bot.result = nullptr;
        auto receipt = ConversationIntake(*store).receive({"owner", true}, {"bot", "", "env", "project", Json::object()},
                                                          std::to_string(++sequence), text);
        runtime->runConversations();
        return receipt.at("messageId");
    }
    Json call(std::string tool, Json args) {
        turn(tool, tool, std::move(args));
        require(bot.result.is_object() && bot.result.value("ok", false), tool + " failed: " + bot.result.dump());
        return bot.result;
    }
    Id start(std::string request) { return call("start_task", {{"request", std::move(request)}}).at("taskId"); }
    void cancel(Id task) { call("cancel_task", {{"taskId", task}, {"generation", Tasks(*store).task(task).task.generation}}); }
    void ticks(int count) {
        for (int i = 0; i < count; ++i) runtime->tick();
    }
};

void taskLaunchUsesPolicyAndRedactsQueuedPrompt() {
    Fixture f;
    PolicyRules policyRules{*f.store};
    SecretRequests secretRequests{*f.store};
    const Person owner{"owner", true};
    policyRules.changeNeverRule(owner, Action::environment_launch, "env", true);
    const std::string secret = "credential-registered-after-queueing";
    const auto task = f.start("Collect the release notes " + secret);
    f.ticks(3);
    const auto wait = taskJson(Tasks(*f.store).task(task)).at("wait");
    require(wait.at("reason") == "settings" && f.t3.commands.empty(), "denied task launch reached T3");
    const auto input = secretRequests.requestSecret("bot", {"release-credential"});
    secretRequests.submitSecret(owner, "bot", input, secret);
    policyRules.changeNeverRule(owner, Action::environment_launch, "env", false);
    f.now += 5;
    f.t3.loseReceipt = true;
    f.ticks(1);
    require(f.t3.commands.size() == 1 && f.t3.commands[0].instruction.find(secret) == std::string::npos &&
                f.t3.commands[0].instruction.find("Collect the release notes [redacted]") != std::string::npos,
            "the task prompt included newly protected text or lost the request");
    f.now += 5;
    f.ticks(2);
    require(f.t3.commands.size() == 1 && Tasks(*f.store).task(task).task.state == TaskState::running,
            "the redacted task launch was not reconciled");
}

void expansionRedactsBeforeSearchAndPaging() {
    Fixture f;
    const std::string secret = "secret-value-for-compaction";
    const std::string prefix(1995, 'x');
    const auto original = f.turn(prefix + secret + " tail");
    const auto compacted = f.call("compact_conversation", {{"endMessageId", original}, {"summary", "Notes: " + secret}});
    const auto compaction = compacted.at("compaction").at("compactionId");
    SecretRequests secretRequests{*f.store};
    secretRequests.submitSecret({"owner", true}, "bot", secretRequests.requestSecret("bot", {"context-credential"}), secret);
    const auto first = f.call("expand_context", {{"compactionId", compaction}, {"startMessageId", original}});
    const auto second = f.call("expand_context", {{"cursor", first.at("nextCursor")}});
    require(first.at("messages").at(0).at("text").get<std::string>() + second.at("messages").at(0).at("text").get<std::string>() ==
                prefix + "[redacted] tail",
            "paging split the protected value before redaction");
    require(f.bot.history.front().at("summary") == "Notes: [redacted]", "protected value reached the compacted model history");
    require(f.call("expand_context", {{"compactionId", compaction}, {"query", "value-for-compaction"}}).at("messages").empty(),
            "expansion search matched a fragment of the protected value");
}

void expansionRejectsStaleCursors() {
    Fixture f;
    const std::string secret = "fixture-" + std::string(64, 'k');
    const std::string instruction = " DO-NOT-PUBLISH ";
    const auto originalText = secret + std::string(1928, 'x') + "根" + instruction + std::string(2500, 'y');
    const auto original = f.turn(originalText);
    const auto saved = f.call("compact_conversation", {{"endMessageId", original}, {"summary", "Keep the instruction"}});
    const auto compaction = saved.at("compaction").at("compactionId");
    const Json range{{"compactionId", compaction}, {"startMessageId", original}, {"endMessageId", original}};
    const auto first = f.call("expand_context", range);
    auto continuation = range;
    continuation["cursor"] = first.at("nextCursor");
    f.restart();
    const auto text = [](const Json& page) { return page.at("messages").at(0).at("text").get<std::string>(); };
    const auto continued = text(f.call("expand_context", continuation));
    require(!continued.empty() && std::string_view(originalText).substr(text(first).size()).starts_with(continued),
            "An unchanged projection did not resume after restart");

    bool checked = false;
    f.bot.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return {"expand_context", range, "Read the first page"};
        if (steps.size() == 1) {
            SecretRequests secretRequests{*f.store};
            secretRequests.submitSecret({"owner", true}, "bot", secretRequests.requestSecret("bot", {"paging-fixture"}), secret);
            return {"expand_context", continuation, "Continue the first page"};
        }
        require(steps.at(0).at("result").at("cursorStale") == true && !steps.at(0).at("result").contains("messages") &&
                    steps.at(1).at("result").at("ok") == false,
                "A saved page was replayed after the active redaction view changed");
        checked = true;
        return {"reply", {{"text", "I will start a fresh read"}}, "Acknowledge the stale cursor"};
    };
    f.turn("Detect redaction changes within the turn");
    f.bot.conversation = {};
    require(checked, "The test did not reach the model with the stale page");
    f.turn("Continue the saved expansion", "expand_context", continuation);
    require(f.bot.result.at("ok") == false && !f.bot.result.contains("messages"),
            "A cursor from a different redaction view returned a page");
    const auto redacted = f.call("expand_context", range);
    continuation["cursor"] = redacted.at("nextCursor");
    const auto second = f.call("expand_context", continuation);
    const auto third =
        f.call("expand_context", {{"compactionId", compaction}, {"endMessageId", original}, {"cursor", second.at("nextCursor")}});
    require(text(redacted) + text(second) + text(third) ==
                    "[redacted]" + std::string(1928, 'x') + "根" + instruction + std::string(2500, 'y') &&
                redacted.at("nextCursor").is_number_integer(),
            "A fresh expansion skipped an instruction, included secret text, or embedded text in its reference");
}

void expansionCursorKeepsItsSearchRange() {
    Fixture f;
    f.turn("ALPHA evidence one");
    const auto lastMatch = f.turn("ALPHA evidence two");
    const auto boundary = f.turn("Unrelated entry at the end");
    f.call("compact_conversation", {{"endMessageId", boundary}, {"summary", "Collate the evidence"}});
    const auto first = f.call("expand_context", {{"query", "ALPHA"}, {"limit", 1}});
    const auto cursor = first.at("nextCursor");
    const auto wider = f.turn("ALPHA evidence added later");
    f.call("compact_conversation", {{"endMessageId", wider}, {"summary", "Collate the new evidence"}});
    f.turn("Continue with a smaller range", "expand_context", {{"cursor", cursor}, {"endMessageId", lastMatch}});
    require(f.bot.result.at("ok") == false, "A cursor accepted a smaller search range");
    const auto continued = f.call("expand_context", {{"cursor", cursor}});
    require(continued.at("messages").size() == 1 && continued.at("messages").at(0).at("messageId") == lastMatch &&
                !continued.contains("nextCursor"),
            "A cursor resumed a wider range or lost its search filter");
}

void overlapKeepsEachSearchFilter() {
    Fixture f;
    const auto message = f.turn("ALPHA BETA " + std::string(3000, 'x'));
    const auto end = f.turn("ALPHA detail");
    f.call("compact_conversation", {{"endMessageId", end}, {"summary", "Two searches"}});
    const auto alpha = f.call("expand_context", {{"query", "ALPHA"}});
    const auto beta = f.call("expand_context", {{"query", "BETA"}});
    require(alpha.at("messages") == beta.at("messages") && alpha.at("nextCursor") != beta.at("nextCursor"),
            "the fixture did not issue independent cursors for the overlapping search result");
    f.turn("Continue overlapping search", "expand_context", {{"cursor", alpha.at("nextCursor")}, {"query", "BETA"}});
    require(f.bot.result.at("ok") == false, "a cursor accepted a different search filter");
    const auto alphaPage = f.call("expand_context", {{"cursor", alpha.at("nextCursor")}});
    const auto betaPage = f.call("expand_context", {{"cursor", beta.at("nextCursor")}});
    require(alphaPage.at("messages").back().at("messageId") == end && betaPage.at("messages").size() == 1 &&
                betaPage.at("messages").at(0).at("messageId") == message,
            "the overlapping searches lost their own filters");
}

void freshStandaloneSessionIncludesEarlierEvidence() {
    Fixture f;
    const auto sourceTask = f.start("Compare the options");
    f.ticks(3);
    f.t3.finish("run-1", "Option B must preserve the retry window");
    f.ticks(2);
    const auto history = TaskHistory(*f.store).history(sourceTask);
    const auto report = std::ranges::find_if(history, [](const Json& item) { return item.at("kind") == "report"; });
    require(report != history.end(), "fixture did not save the source report");
    const Id task =
        f.call("start_task",
               {{"request", "Implement the chosen option"}, {"doneWhen", "Tests pass"}, {"sourceResultMessageId", report->at("id")}})
            .at("taskId");
    for (int i = 0; i < 12 && !Tasks(*f.store).task(task).task.activeRun; ++i) f.ticks(1);
    const auto before = Tasks(*f.store).task(task).task;
    const auto earlierHistory = TaskHistory(*f.store).history(task);
    f.call("continue_fresh_session", {{"taskId", task}, {"instruction", "Repeat checks with fresh context"}});
    f.ticks(3);
    require(f.t3.commands.size() == 2 && f.t3.interrupts.contains("run-2"), "fresh handoff bypassed settlement");
    f.t3.finish("run-2", "Latest report: " + std::string(9000, 'z'), "interrupted");
    f.ticks(5);
    const auto& next = f.t3.commands.back();
    const auto after = Tasks(*f.store).task(task).task;
    require(f.t3.commands.size() == 3 && next.tool == CommandTool::delegate &&
                next.instruction.find("Option B must preserve the retry window") != std::string::npos &&
                next.instruction.find("Latest report:") != std::string::npos,
            "fresh Session lost the linked result or the interrupted Run's report");
    require(after.doneWhen == before.doneWhen && after.currentRevision == before.currentRevision &&
                after.activeRun.value().ref.threadId == "thread-3" &&
                TaskHistory(*f.store).history(task, 0, static_cast<unsigned>(earlierHistory.size())) == earlierHistory,
            "fresh continuation lost the task's criteria, revision, or history");
}

void taskRequestKeepsEmbeddedNul() {
    Fixture f;
    std::string request(1, '\0');
    request += "Compare the bytes after the marker";
    const Id id = f.call("start_task", {{"request", request}, {"doneWhen", request}}).at("taskId");
    f.ticks(3);
    const auto task = taskJson(Tasks(*f.store).task(id));
    require(task["request"] == request && task["intent"] == request && task["doneWhen"] == request,
            "task request changed before the first revision was saved");
    require(f.t3.commands.size() == 1 && f.t3.commands.front().instruction.find("Task intent:\n" + request + "\n\nCompletion criteria:\n" +
                                                                                request) != std::string::npos,
            "task dispatch lost the request bytes after the marker");
}

void cancelledTaskNotificationsDistinguishFailures() {
    for (const auto status : {"interrupted", "failed"}) {
        Fixture f;
        const auto id = f.start("Implement feature");
        f.ticks(3);
        f.cancel(id);
        const auto cursor = Notifications(*f.store).notificationEventsSince(localOwner, {"bot"}, std::nullopt, {});
        f.t3.finish("run-1", "The Session ended", status);
        f.ticks(3);
        require(f.t3.interrupts.empty() && Tasks(*f.store).task(id).task.state == TaskState::cancelled,
                "the stop-race fixture did not settle before an interrupt acknowledgement");
        const auto page = Notifications(*f.store).notificationEventsSince(localOwner, {"bot"}, cursor.through, cursor.scope);
        if (std::string_view(status) == "interrupted")
            require(page.events.empty(), "an interrupted task emitted a failure alert before the stop was acknowledged");
        else
            require(page.events.size() == 1 && page.events[0]["kind"] == "failure", "a pending stop hid a genuine task failure");
    }
}

void settledWhileReadingInstruction() {
    Fixture f;
    const auto task = f.start("Preserve duplicate input rows");
    f.ticks(3);
    f.t3.finish("run-1", "Keep the original row count");
    f.t3.onObserve = [&] {
        ConversationIntake(*f.store).receive({"owner", true}, {"bot", "", "env", "project", Json::object()}, "fresh",
                                             "Continue in a fresh Session");
        f.bot.action = {"continue_fresh_session", {{"taskId", task}}, "Continue the same task"};
    };
    f.ticks(1);
    require(Tasks(*f.store).task(task).task.state == TaskState::deciding, "the task closed before the pending message was read");
    f.restart();
    f.ticks(6);
    const auto& next = f.t3.commands.back();
    require(f.t3.commands.size() == 2 && next.tool == CommandTool::delegate && Tasks(*f.store).task(task).runs.size() == 2 &&
                next.instruction.find("Keep the original row count") != std::string::npos &&
                next.instruction.find("run-1") != std::string::npos &&
                next.instruction.find("Preserve duplicate input rows") != std::string::npos,
            "fresh Session lost its settled report or original intent");
    f.t3.finish("run-2", "The input rows were preserved");
    f.ticks(2);
    f.runtime->runConversations();
    require(Tasks(*f.store).task(task).task.state == TaskState::completed, "the fresh Session's result did not finish the task");
}

void sessionQuestionReachesThePerson() {
    Fixture f;
    const auto task = f.start("Ask for the output format");
    std::vector<std::string> reasons;
    f.bot.conversation = [&](const ModelInput& input) -> Decision {
        if (input.event.contains("sessionWake")) reasons.push_back(input.event.at("sessionWake").at("reason"));
        return {"reply", {{"text", "Noted"}}, "Tell the person"};
    };
    f.ticks(3);
    auto& run = f.t3.observations["run-1"];
    run.kind = Observation::Kind::Question;
    run.question = {"q", {{.id = "choice", .prompt = "Which format?"}}};
    f.ticks(3);
    const auto asked = Tasks(*f.store).task(task);
    require(asked.wait && asked.wait->reason() == WaitReason::user_question && asked.wait->text == "Which format?" &&
                f.t3.commands.size() == 1 && reasons.empty(),
            "a Session question did not reach the person as the Session asked it");
    f.t3.finish("run-1", "Used the default format");
    f.ticks(3);
    f.runtime->runConversations();
    require(reasons == std::vector<std::string>{"finished"} && Tasks(*f.store).task(task).task.state == TaskState::completed &&
                f.t3.commands.size() == 1,
            "the finished Run did not wake the conversation and close the task");
}

void stopBeforeFirstSend() {
    for (const bool prepared : {false, true}) {
        Fixture f;
        const auto id = f.start("Pending launch");
        if (prepared) {
            f.ticks(2);
            const auto action = Tasks(*f.store).task(id).pendingAction;
            require(action && action->phase == OutboxPhase::ready, "the fixture did not prepare a launch");
        }
        f.cancel(id);
        f.restart();
        f.ticks(3);
        require(f.t3.commands.empty() && Tasks(*f.store).task(id).task.state == TaskState::cancelled,
                "cancellation admitted pending or prepared work");
    }
}

void scopedCompactionPages() {
    Fixture f;
    std::string original;
    // Control characters grow sixfold in JSON, so one message of ordinary size spans several evidence pages.
    while (original.size() + 16 < 15'900) original += "根\x01\x01\x01\x01\x01\x01\x01\x01";
    const Id first = f.turn(original);
    const Id end = f.turn("Boundary");
    const auto summary = f.call("compact_conversation", {{"endMessageId", end}, {"summary", "Keep the original evidence"}});
    require(summary["compaction"]["originalCount"] == 3, "summary counted messages outside its boundary");
    f.bot.action = {"expand_context", {{"compactionId", summary["compaction"]["compactionId"]}}, "Look up original evidence"};
    ConversationIntake(*f.store).receive({"owner", true}, {"other", "", "env", "project", Json::object()}, "read",
                                         "Read first Bot's evidence");
    f.runtime->runConversations();
    require(f.bot.result.at("ok") == false && f.bot.history.empty(), "compaction crossed Bot histories");
    std::string replay;
    Json args = {{"startMessageId", first}, {"endMessageId", first}, {"limit", 1}};
    while (true) {
        const auto page = f.call("expand_context", args);
        require(page.dump().size() <= maxExpandedBytes && page.at("messages").size() == 1 && page["messages"][0]["messageId"] == first,
                "escaped evidence exceeded its byte or item cap");
        const auto chunk = page["messages"][0]["text"].get<std::string>();
        require(!chunk.empty() && replay.size() + chunk.size() <= original.size(), "non-advancing evidence cursor");
        replay += chunk;
        if (!page.contains("nextCursor")) break;
        args["cursor"] = page.at("nextCursor");
    }
    require(replay == original, "UTF-8 cursor skipped or duplicated original bytes");

    int calls = 0;
    bool reviewed = false;
    f.bot.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = input.event.at("turn");
        std::size_t expanded = 0;
        for (const auto& step : steps) {
            const auto& result = step.at("result");
            if (result.contains("messages")) expanded += result.dump().size();
        }
        require(expanded <= maxExpandedBytes && expanded + input.history.dump().size() <= maxBytes,
                "expansions accumulated beyond model context budget");
        if (steps.empty() || !steps.front()["result"].value("evidenceOmitted", false)) {
            require(++calls <= 40, "repeated expansions never left the evidence budget");
            return {"expand_context", {{"startMessageId", first}, {"endMessageId", first}}, "Read original evidence"};
        }
        require(steps.back()["result"].contains("messages"), "bounded context lost the latest requested evidence");
        reviewed = true;
        return {"reply", {{"text", "Reviewed"}}, "Finish"};
    };
    f.turn("Read several pages");
    require(calls > 1 && reviewed, "multi-step expansion did not respect the evidence budget");
}

void historyKeepsLargestFittingPrefix() {
    Fixture f;
    std::string original;
    constexpr std::size_t filler = 15'000;
    while (original.size() + 16 < filler) original += "根拠\n\t\"\\\x01";
    const Id target = f.turn(original);
    for (std::size_t used = filler; used + filler < maxBytes; used += filler) f.turn(std::string(filler, 'x'));
    f.turn("Inspect history near its byte limit");
    auto history = f.bot.history;
    const auto partial = std::ranges::find_if(history, [&](const Json& entry) { return entry["messageId"] == target; });
    require(partial != history.end(), "history dropped a message whose prefix fit");
    const auto excerpt = partial->at("text").get<std::string>();
    require(!excerpt.empty() && excerpt.size() < original.size() && original.starts_with(excerpt) && history.dump().size() <= maxBytes,
            "history prefix did not fit the serialized byte budget");
    auto next = excerpt.size() + 1;
    while ((static_cast<unsigned char>(original.at(next)) & 0xc0) == 0x80) ++next;
    (*partial)["text"] = original.substr(0, next);
    require(history.dump().size() > maxBytes, "the next character should exceed the remaining serialized bytes");
}

void compactConversationAcrossRestart() {
    Fixture f;
    const std::string original = "Keep the original instruction and release checksum";
    const auto first = f.turn(original);
    const auto end = f.turn("Evidence " + std::string(3400, 'x'));
    f.turn("Newer direction");
    const auto compact = f.call("compact_conversation", {{"endMessageId", end}, {"summary", "Keep the tested revision."}});
    f.restart();
    f.turn("Inspect history");
    require(f.bot.history.front().at("compactionId") == compact["compaction"]["compactionId"] &&
                f.bot.history.front().at("startMessageId") == first && f.bot.history.front().at("endMessageId") == end &&
                f.bot.history.dump().find("Newer direction") != std::string::npos &&
                f.bot.history.dump().find(original) == std::string::npos,
            "model history does not respect compaction");
    require(Conversations(*f.store).conversation("bot").dump().find(original) != std::string::npos, "audit history was lost");
    auto expanded = f.call("expand_context", {{"query", "release checksum"}, {"limit", 100}});
    require(expanded.at("messages").size() == 1 && expanded["messages"][0]["text"] == original, "missing original evidence");
    expanded = f.call("expand_context", {{"compactionId", compact["compaction"]["compactionId"]}, {"startMessageId", end}});
    require(expanded.dump().size() <= maxExpandedBytes && expanded.contains("nextCursor") &&
                expanded.at("messages").at(0).at("messageId") == end,
            "unbounded expansion or missing cursor");
    f.turn("Invalid boundary", "compact_conversation", {{"endMessageId", first}, {"summary", "Discard coverage"}});
    require(f.bot.result.at("ok") == false, "accepted shrinking boundary");
    f.turn("Same boundary", "compact_conversation", {{"endMessageId", end}, {"summary", std::string(3200, 'z')}});
    require(f.bot.result.at("ok") == false && Conversations(*f.store).compactions("bot").size() == 1 &&
                Conversations(*f.store).compactions("bot").at(0) == compact.at("compaction"),
            "A same-boundary summary replaced the durable compaction");
    const auto nextBoundary = f.turn("New evidence to include");
    const auto advanced = f.call("compact_conversation", {{"endMessageId", nextBoundary}, {"summary", "Include newer evidence"}});
    require(advanced.at("compaction").at("endMessageId") == nextBoundary && Conversations(*f.store).compactions("bot").size() == 2,
            "a valid compaction did not advance coverage");
}
}

int main() {
    try {
        taskLaunchUsesPolicyAndRedactsQueuedPrompt();
        expansionRedactsBeforeSearchAndPaging();
        expansionRejectsStaleCursors();
        expansionCursorKeepsItsSearchRange();
        overlapKeepsEachSearchFilter();
        freshStandaloneSessionIncludesEarlierEvidence();
        taskRequestKeepsEmbeddedNul();
        cancelledTaskNotificationsDistinguishFailures();
        settledWhileReadingInstruction();
        sessionQuestionReachesThePerson();
        stopBeforeFirstSend();
        scopedCompactionPages();
        historyKeepsLargestFittingPrefix();
        compactConversationAcrossRestart();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
