#include "flock/board_inbox.hpp"
#include "flock/memory_automation.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <future>
#include <iostream>
#include <unistd.h>

using namespace flockv0;
namespace {
const Person owner{"owner", true};
const Intake alpha{"alpha", "", "env", "alpha-project"}, beta{"beta", "", "env", "beta-project"};

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function> void rejectsWithConflict(Function run, std::string_view expected) {
    try {
        run();
    } catch (const Conflict& error) {
        check(error.what() == expected, "Unexpected conflict message");
        return;
    }
    throw std::runtime_error("Missing conflict: " + std::string(expected));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        char name[] = "/tmp/flock-memory-XXXXXX";
        const auto made = mkdtemp(name);
        check(made != nullptr, "Temporary directory creation failed");
        path = made;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct Adapter : T3 {
    std::string authGeneration(const std::string&, const Json&) override { return {}; }
    Delivery send(const Command&) override { throw std::runtime_error("Unexpected Session launch"); }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};

struct Script : Model {
    std::vector<Decision> choices;
    std::vector<ModelInput> inputs;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        inputs.push_back(input);
        if (!choices.empty()) {
            check(input.tools.end() != std::find(input.tools.begin(), input.tools.end(), choices.front().tool),
                  "Conversation tool was not advertised");
            auto choice = std::move(choices.front());
            choices.erase(choices.begin());
            return choice;
        }
        return {"ask_user", {{"question", "Anything else?"}}, "Fixture task"};
    }
};

Decision reply() {
    return {"reply", {{"text", "Done"}}, "Confirmed"};
}

Decision remember(Id version, std::string_view content, std::string_view summary, std::string_view quote) {
    return {"remember_preference",
            {{"baseVersion", version}, {"content", content}, {"summary", summary}, {"sourceQuote", quote}},
            "User preference"};
}

const Json& lastResult(const ModelInput& input) {
    return input.event.at("turn").back().at("result");
}

void memoryFlow(const std::filesystem::path& db) {
    Store store(db, [](std::string_view id) { return id == "alpha" ? "Prefer CSV.\n" : "Beta private."; });
    auto policy = store.policy();
    const std::string path = "team/staging.md";

    Adapter adapter;
    Script bot;
    RuntimeOptions options;
    options.clock = [] { return Seconds{1'767'456'000}; };
    Runtime runtime(store, bot, adapter, {}, options);
    const auto turn = [&](const Intake& target, std::string_view id, std::vector<Decision> choices) {
        const auto start = bot.inputs.size();
        choices.push_back(reply());
        bot.choices = std::move(choices);
        store.receive(localOwner, target, id, id);
        runtime.runConversations();
        check(bot.choices.empty(), "Conversation did not complete");
        return start;
    };
    const auto step = turn(
        alpha, "Remember to use staging-eu for staging checks",
        {
            {"read_memory", Json::object(), "Read current conventions"},
            {"propose_memory_edit",
             {{"baseVersion", 0}, {"content", "Prefer CSV.\nUse staging-eu.\n"}, {"rationale", "User specified the staging environment"}},
             "Propose convention"},
        });
    check(lastResult(bot.inputs.at(step + 1)).at("version") == 0 && lastResult(bot.inputs.at(step + 1)).at("content") == "Prefer CSV.\n",
          "Private memory did not come from notes");
    const auto proposal = lastResult(bot.inputs.at(step + 2)).at("proposalId");
    const auto apply = turn(alpha, "Apply the convention",
                            {
                                {"apply_memory_edit", {{"baseVersion", 0}, {"proposalId", proposal}}, "Apply"},
                                {"memory_history", Json::object(), "Verify change"},
                            });
    const auto history = lastResult(bot.inputs.at(apply + 2));
    const auto first = history.at("revisions").at(0);
    check(bot.inputs.at(apply + 1).notes.memory == "Prefer CSV.\nUse staging-eu.\n",
          "An active conversation used memory from before its own edit");
    check(first.at("author") == "bot:alpha" && first.at("diffSummary") == "-0 +1 lines" &&
              history.at("proposals").at(0).at("status") == "applied",
          "Memory proposal and diff were not recorded");
    const auto followup = turn(alpha, "Which staging environment is standard?", {});
    check(bot.inputs.at(followup).notes.memory == "Prefer CSV.\nUse staging-eu.\n", "BotNotes did not load edited memory");

    store.memory(owner, "alpha", "revert_memory_edit", {{"baseVersion", 1}, {"revisionId", first.at("id")}});
    const auto restored = store.memory(owner, "alpha", "read_memory", Json::object());
    const auto revisions = store.memory(owner, "alpha", "memory_history", Json::object()).at("revisions");
    check(restored.at("content") == "Prefer CSV.\n" && restored.at("version") == 2 && revisions.size() == 2 &&
              revisions.at(0).at("revertedFrom") == first.at("id") && revisions.at(0).at("author") == "person:owner",
          "Revert did not preserve the original revision and restore content");

    const Person member{"memory-member", false};
    policy.setMember(localOwner, "alpha", member.id, true);
    bool sharedDenied = false;
    try {
        store.memory(member, "alpha", "read_memory", {{"docId", path}});
    } catch (const AccessDenied&) {
        sharedDenied = true;
    }
    check(sharedDenied, "Bot membership granted shared memory access");
    const auto memberStep = bot.inputs.size();
    bot.choices = {{"apply_memory_edit", {{"baseVersion", 2}, {"content", "Member edit"}}, "Change memory"},
                   {"remove_schedule", {{"id", 1}}, "Remove schedule"},
                   {"update_bot_settings", {{"environmentId", alpha.environmentId}, {"runTimeoutSeconds", 1}}, "Change timeout"},
                   reply()};
    store.receive(member, alpha, "member-mutation", "Change memory and schedules");
    runtime.runConversations();
    check(lastResult(bot.inputs.at(memberStep + 1)).at("error") == "Memory access denied" &&
              lastResult(bot.inputs.at(memberStep + 2)).at("error") == "Automation change denied" &&
              lastResult(bot.inputs.at(memberStep + 3)).at("error") == "Bot settings access denied",
          "Member chat bypassed owner-only mutations");

    const auto shared =
        turn(alpha, "Update the staging playbook",
             {
                 {"apply_memory_edit", {{"docId", path}, {"baseVersion", 0}, {"content", "Staging playbook"}}, "Update shared file"},
             });
    check(lastResult(bot.inputs.at(shared + 1)).at("ok") == true, "Writable shared file denied");
    const auto reader = turn(beta, "Read the staging playbook",
                             {
                                 {"read_memory", {{"docId", path}}, "Read allowed file"},
                                 {"read_memory", Json::object(), "Read my private memory"},
                                 {"read_memory", {{"docId", "bot:alpha:memory"}}, "Try another Bot's private key"},
                             });
    check(lastResult(bot.inputs.at(reader + 1)).at("content") == "Staging playbook" &&
              lastResult(bot.inputs.at(reader + 2)).at("content") == "Beta private." &&
              lastResult(bot.inputs.at(reader + 3)).at("content") == "",
          "Shared or private memory boundary violated");
    const auto spoof = turn(beta, "Inspect the shared key",
                            {
                                {"read_memory", {{"docId", "bot:alpha:memory"}}, "Read shared key"},
                            });
    check(lastResult(bot.inputs.at(spoof + 1)).at("docKey") == "shared:bot:alpha:memory" &&
              lastResult(bot.inputs.at(spoof + 1)).at("content") == "",
          "Read access to a shared key exposed private memory");
    policy.changeNeverRule(owner, Action::shared_memory_read, path, true);
    policy.changeNeverRule(owner, Action::shared_memory_write, path, true);
    const Intake gamma{"gamma", "", "env", "gamma-project"};
    const auto denied =
        turn(gamma, "Try the staging playbook",
             {
                 {"read_memory", {{"docId", path}}, "Try unauthorized read"},
                 {"apply_memory_edit", {{"docId", path}, {"baseVersion", 1}, {"content", "Unauthorized"}}, "Try unauthorized write"},
             });
    check(lastResult(bot.inputs.at(denied + 1)).at("ok") == false && lastResult(bot.inputs.at(denied + 2)).at("ok") == false,
          "Blocked Bot accessed a shared file");

    policy.changeNeverRule(owner, Action::shared_memory_read, path, false);
    policy.changeNeverRule(owner, Action::shared_memory_write, path, false);
    Store left(db), right(db);
    std::barrier start(2);
    const auto personEdit = [&](Store& connection, const Person& person, const Intake& target, std::string_view content) {
        start.arrive_and_wait();
        try {
            connection.memory(person, target.botId, "apply_memory_edit", {{"docId", path}, {"baseVersion", 1}, {"content", content}});
            return true;
        } catch (const Conflict& error) {
            check(std::string_view(error.what()).starts_with("Memory version conflict"), "Wrong concurrent edit failure");
            return false;
        }
    };
    const Person editor{"editor", true};
    auto firstEdit = std::async(std::launch::async, [&] { return personEdit(left, owner, alpha, "Alpha version"); });
    auto secondEdit = std::async(std::launch::async, [&] { return personEdit(right, editor, beta, "Beta version"); });
    const auto alphaWon = firstEdit.get(), betaWon = secondEdit.get();
    check(alphaWon != betaWon, "Concurrent edits did not have exactly one winner");
    const auto final = store.memory(owner, "alpha", "read_memory", {{"docId", path}});
    check(final.at("version") == 2 && final.at("content") == (alphaWon ? "Alpha version" : "Beta version"),
          "A concurrent shared edit silently overwrote the winner");
    const auto stale =
        turn(beta, "Try a stale shared edit",
             {
                 {"apply_memory_edit", {{"docId", path}, {"baseVersion", 1}, {"content", "Stale edit"}}, "Apply stale snapshot"},
             });
    check(lastResult(bot.inputs.at(stale + 1)).at("error").get<std::string>().starts_with("Memory version conflict"),
          "Bot accepted a stale shared version");

    const auto secret = policy.requestSecret("beta", {"deployment-token"});
    policy.submitSecret(owner, "beta", secret, "fixture-protected-token");
    const auto redact = turn(alpha, "Record a sanitized staging example",
                             {
                                 {"apply_memory_edit",
                                  {{"docId", path}, {"baseVersion", 2}, {"content", "Example: fixture-protected-token"}},
                                  "Example includes fixture-protected-token"},
                             });
    check(bot.inputs.at(redact + 1).event.dump().find("fixture-protected-token") == std::string::npos,
          "Shared-memory decision leaked a protected value in the conversation trace");
    const auto sharedContent = store.memory(owner, "beta", "read_memory", {{"docId", path}});
    check(sharedContent.at("content") == "Example: [redacted]", "Shared text exposed another Bot's protected value");

    auto limited = beta;
    limited.config["modelCallLimit"] = 1;
    bot.choices = {{"read_memory", {{"docId", path}}, "Check the playbook"}, reply()};
    const auto receipt = store.receive(localOwner, limited, "paused-shared-read", "Check the shared playbook");
    runtime.runConversations();
    const Id pauseId = store.pausedConversations(beta.botId).at(0).at("messageId");
    policy.changeNeverRule(owner, Action::shared_memory_read, path, true);
    store.decideInbox(owner, {beta.botId, InboxKind::conversation_model_budget, receipt.at("messageId"), pauseId, 0, 0, pauseId},
                      "approve");
    runtime.runConversations();
    check(bot.choices.empty() && lastResult(bot.inputs.back()).at("ok") == false &&
              bot.inputs.back().event.dump().find("Example: [redacted]") == std::string::npos,
          "Resuming after revocation exposed a cached shared-memory read");
}

void proactiveMemory(const std::filesystem::path& db) {
    constexpr auto seed = "Release-note format: CSV.\n";
    constexpr auto preference = "I prefer release notes under one page.";
    constexpr auto preferred = "Release-note format: CSV.\nKeep release notes under one page.\n";
    constexpr auto correction = "Use Markdown instead of CSV for future release notes.";
    constexpr auto corrected = "Release-note format: Markdown.\nKeep release notes under one page.\n";
    Store store(db, [seed](auto) { return seed; });
    Script model;
    Adapter adapter;
    Runtime runtime(store, model, adapter, {});
    const auto send = [&](std::string_view key, std::string_view message, Decision edit) {
        model.choices = {{"review_preference", Json::object(), "Review a lasting preference"}, std::move(edit), reply()};
        store.receive(localOwner, alpha, key, message);
        runtime.runConversations();
    };
    const auto notices = [&] {
        Json result = Json::array();
        for (const auto& event : store.conversation(alpha.botId))
            if (event.at("kind") == "memory_remembered") result.push_back(event.at("data"));
        return result;
    };
    const auto memory = [&] { return store.memory(owner, alpha.botId, "read_memory", Json::object()); };

    send("preferred", preference, remember(0, preferred, "Keep release notes under one page.", preference));
    auto saved = notices();
    check(model.choices.empty() && saved.size() == 1 && saved.front().at("text") == "Keep release notes under one page." &&
              saved.front().at("canUndo") == true && memory().at("content") == preferred && memory().at("version") == 1,
          "A direct preference did not update memory and show an Undo notice");
    const auto firstRevision = saved.front().at("revisionId");

    send("same-preference", preference, remember(1, preferred, "Keep release notes under one page.", preference));
    check(model.choices.empty() && notices().size() == 1 && memory().at("version") == 1,
          "An unchanged preference created another revision or notice");

    send("skip-preference", "What is the release date?", {"skip_preference", Json::object(), "No lasting preference"});
    check(model.choices.empty() && notices().size() == 1 && memory().at("version") == 1,
          "Skipping an unrelated message changed memory or interrupted the reply");

    send("unrelated", "What is the release date?", remember(1, corrected, correction, correction));
    check(model.choices.empty() && lastResult(model.inputs.back()).at("ok") == false && memory().at("content") == preferred &&
              notices().size() == 1,
          "A preference missing from the person's message changed memory");

    auto policy = store.policy();
    policy.setMode(owner, "supervised");
    policy.changeRule(owner, RuleKind::ask, Action::private_memory_write, alpha.botId, true);
    send("correction", correction, remember(1, corrected, "Use Markdown for release notes.", correction));
    const auto pending = store.inbox(localOwner);
    check(pending.size() == 1 && pending.front().at("status") == "pending" &&
              pending.front().at("prompt") == "\"Use Markdown for release notes.\"" && notices().size() == 1 && memory().at("version") == 1,
          "A private memory correction bypassed approval");
    const auto& item = pending.front();
    store.decideInbox(localOwner,
                      {alpha.botId, InboxKind::policy_approval, item.at("itemId"), item.at("waitId"), item.at("taskId"),
                       item.at("targetRevision"), item.at("targetGeneration")},
                      "allow_once");
    runtime.runConversations();
    saved = notices();
    check(model.choices.empty() && saved.size() == 2 && saved.back().at("text") == "Use Markdown for release notes." &&
              !saved.front().value("canUndo", false) && saved.back().at("canUndo") == true && memory().at("content") == corrected,
          "The approved correction did not replace the old preference");
    rejectsWithConflict(
        [&] {
            store.memory(owner, alpha.botId, "revert_memory_edit",
                         {{"baseVersion", saved.front().at("version")}, {"revisionId", firstRevision}});
        },
        "Memory version conflict: expected 1, current 2");

    const auto revision = saved.back().at("revisionId");
    store.memory(owner, alpha.botId, "revert_memory_edit", {{"baseVersion", saved.back().at("version")}, {"revisionId", revision}});
    const auto reverted = notices();
    Store reopened(db);
    const auto history = reopened.memory(owner, alpha.botId, "memory_history", Json::object()).at("revisions");
    check(memory().at("content") == preferred && reverted.size() == 2 && reverted.back().at("undone") == true &&
              !reverted.back().value("canUndo", false) && history.size() == 3 && history.front().at("revertedFrom") == revision,
          "Undo did not persist the restored memory and identify the reverted edit");
}

void unavailablePreferenceReviewStillReplies(const std::filesystem::path& db) {
    Store store(db, [](auto) { return "Prefer CSV."; });
    auto policy = store.policy();
    policy.submitSecret(owner, alpha.botId, policy.requestSecret(alpha.botId, {"fixture-token"}), "memory");
    Script model;
    Adapter adapter;
    Runtime runtime(store, model, adapter, {});
    model.choices = {{"review_preference", Json::object(), "Review a lasting preference"}, reply()};
    const auto receipt = store.receive(localOwner, alpha, "unavailable-review", "I prefer short release notes.");
    runtime.runConversations();

    const auto events = store.conversation(alpha.botId);
    check(model.choices.empty() && model.inputs.size() == 2 && model.inputs.back().event.at("kind") == "conversation_turn" &&
              std::ranges::any_of(events,
                                  [&](const auto& event) {
                                      return event.at("kind") == "conversation_reply" &&
                                             event.at("data").at("replyToMessageId") == receipt.at("messageId") &&
                                             event.at("data").at("text") == "Done";
                                  }),
          "Unavailable preference review interrupted the ordinary reply");
}

void memoryRedactionLimit(const std::filesystem::path& db) {
    Store store(db, [](auto) { return "Original"; });
    auto policy = store.policy();
    const auto secret = policy.requestSecret(alpha.botId, {"short-fixture-value"});
    policy.submitSecret(owner, alpha.botId, secret, "z9q7");

    const std::string suffix(8192 - 10, 'x');
    const Json tooLong{{"baseVersion", 0}, {"content", "z9q7" + suffix + "tail"}};
    rejectsWithConflict([&] { store.memory(owner, alpha.botId, "apply_memory_edit", tooLong); },
                        "Redacted memory content exceeds 8192 bytes");
    const auto current = store.memory(owner, alpha.botId, "read_memory", Json::object());
    const auto history = store.memory(owner, alpha.botId, "memory_history", Json::object());
    check(current.at("version") == 0 && current.at("content") == "Original" && history.at("revisions").empty(),
          "An oversized redacted edit changed the memory document or its history");

    store.memory(owner, alpha.botId, "apply_memory_edit", {{"baseVersion", 0}, {"content", "z9q7" + suffix}});
    const auto saved = store.memory(owner, alpha.botId, "read_memory", Json::object());
    check(saved.at("version") == 1 && saved.at("content") == "[redacted]" + suffix,
          "An edit at exactly 8,192 redacted bytes was not stored intact");
}

Seconds utc(int year, unsigned month, unsigned day, int hour = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>((sys_days{std::chrono::year{year} / month / day} + hours{hour}).time_since_epoch()).count();
}

void configuredRequestLimit(const std::filesystem::path& db) {
    Store store(db);
    const auto start = utc(2026, 1, 1, 12);
    const auto due = utc(2026, 1, 2);
    auto target = alpha;
    target.request = "Daily check";
    ConfiguredSchedule schedule{"daily", "0 0 * * *", "Etc/UTC", target};
    const auto stored = [&] { return store.automations(owner, alpha.botId).at("automations").front(); };
    check(store.syncSchedules(alpha.botId, {schedule}, start).empty(), "Initial schedule import failed");

    schedule.routing.request = std::string(16'000, 'x');
    check(store.syncSchedules(alpha.botId, {schedule}, due + 60).empty() && stored().at("nextRunAt") == due &&
              stored().at("request") == schedule.routing.request,
          "A 16,000-byte request lost its original timing");

    schedule.routing.request += 'y';
    const auto errors = store.syncSchedules(alpha.botId, {schedule}, due + 60);
    check(errors.size() == 1 && errors.front().id == schedule.id && errors.front().message == "Invalid request" &&
              stored().at("status") == "inactive" && stored().at("request").get<std::string>().size() == 16'000,
          "An oversized update with unchanged timing was accepted");
    store.fireDueAutomations(due + 60);
    check(store.tasks().empty(), "A schedule with a rejected request fired");
}

void downtime(const std::filesystem::path& db) {
    auto now = utc(2026, 1, 4);
    const auto first = utc(2026, 1, 5), last = utc(2026, 1, 19), next = utc(2026, 1, 26);
    const auto fireAt = utc(2026, 1, 6, 7);
    Id summaryId, reminderId, canceledId;
    const Json weekly{{"kind", "schedule"}, {"cron", "0 9 * * 1"}, {"timezone", "Asia/Tokyo"}, {"request", "Weekly summary"}};
    {
        Store store(db);
        summaryId = store.createAutomation(owner, alpha, weekly, now).at("id");
        reminderId = store
                         .createAutomation(owner, beta,
                                           {{"kind", "reminder"},
                                            {"timezone", "Asia/Tokyo"},
                                            {"fireAt", fireAt},
                                            {"label", "Staging"},
                                            {"request", "Check staging now"}},
                                           now)
                         .at("id");
        canceledId = store.createAutomation(owner, beta, weekly, now).at("id");
        store.cancelAutomation(owner, beta.botId, canceledId);
        store.fireDueAutomations(now);
        check(store.tasks().empty(), "Automation fired before its target time");
    }
    now = utc(2026, 1, 20, 4);
    const auto tick = [&](Store& store) {
        Script model;
        Adapter adapter;
        RuntimeOptions options;
        options.clock = [&] { return now; };
        Runtime runtime(store, model, adapter, [](auto) { return std::string{}; }, options);
        runtime.tick();
    };
    {
        Store store(db);
        tick(store);
        const auto tasks = taskJson(store.tasks());
        check(tasks.size() == 2, "Downtime did not produce exactly two queued requests");
        const auto recurring = store.tasks("alpha").at(0).task.trigger.value();
        const auto reminder = store.tasks("beta").at(0).task.trigger.value();
        const auto& scheduleDetails = std::get<TimedTaskTrigger>(recurring.details);
        const auto& reminderDetails = std::get<TimedTaskTrigger>(reminder.details);
        check(recurring.id == summaryId && scheduleDetails.kind == TimedTriggerKind::schedule && scheduleDetails.coveredFrom == first &&
                  scheduleDetails.occurrenceCount == 3 && scheduleDetails.targetTime == last,
              "Weekly occurrences were not coalesced into one covered interval");
        check(reminder.id == reminderId && reminderDetails.kind == TimedTriggerKind::reminder && reminderDetails.targetTime == fireAt &&
                  reminderDetails.occurrenceCount == 1,
              "Missed reminder lost its provenance");
        const auto botSchedules = store.automations(owner, "beta").at("automations");
        check(botSchedules.at(0).at("id") == canceledId && botSchedules.at(0).at("status") == "canceled" &&
                  botSchedules.at(1).at("status") == "delivered",
              "Reminder or canceled schedule status incorrect");
        check(store.automations(owner, "alpha").at("automations").at(0).at("nextRunAt") == next,
              "Next weekly summary does not use its configured timezone");
    }
    {
        Store store(db);
        tick(store);
        check(store.tasks().size() == 2, "Restart at identical time fired again");
        now = next - 1;
        tick(store);
        check(store.tasks().size() == 2, "Next weekly summary fired early");
        now = next;
        tick(store);
        tick(store);
        const auto fired = std::get<TimedTaskTrigger>(store.tasks("alpha").at(0).task.trigger.value().details);
        check(store.tasks().size() == 3 && fired.targetTime == next && fired.occurrenceCount == 1,
              "Next weekly occurrence did not fire once");
    }
}

void pausedSchedules(const std::filesystem::path& db) {
    const auto now = utc(2026, 1, 4);
    const auto first = now + 60, resumedAt = now + 5 * 60, next = resumedAt + 60;
    const Json recurring{{"kind", "schedule"}, {"cron", "* * * * *"}, {"timezone", "Etc/UTC"}, {"request", "Recurring check"}};
    auto configured = alpha;
    configured.request = "Configured check";
    const ConfiguredSchedule schedule{"configured", "* * * * *", "Etc/UTC", configured};
    const auto reminder = [&](Store& store, const Intake& target, Seconds fireAt) {
        return store
            .createAutomation(owner, target, {{"kind", "reminder"}, {"timezone", "Etc/UTC"}, {"fireAt", fireAt}, {"request", "Reminder"}},
                              now)
            .at("id");
    };
    Id duringDowntime, afterResume;
    {
        Store store(db);
        store.createAutomation(owner, alpha, recurring, now);
        check(store.syncSchedules(alpha.botId, {schedule}, now + 1).empty(), "Could not import configured schedule");
        reminder(store, alpha, first + 15);
        duringDowntime = reminder(store, alpha, first + 75);
        afterResume = reminder(store, alpha, next + 15);
        reminder(store, beta, first + 15);
        store.setBotPaused(owner, alpha.botId, true);
    }
    {
        Store restarted(db);
        check(restarted.botPaused(alpha.botId), "Bot pause was lost on restart");
        restarted.fireDueAutomations(first + 15);
        check(restarted.tasks(alpha.botId).empty() && restarted.tasks(beta.botId).size() == 1,
              "A paused schedule fired or a different Bot's reminder was skipped");
    }
    {
        Store restarted(db);
        restarted.setBotPaused(owner, alpha.botId, false, [&] { return resumedAt; });
        restarted.fireDueAutomations(resumedAt);
        check(restarted.tasks(alpha.botId).empty(), "Resume replayed missed occurrences");
        const auto schedules = restarted.automations(owner, alpha.botId).at("automations");
        const auto missed = std::ranges::find_if(schedules, [&](const Json& item) { return item.at("id") == duringDowntime; });
        check(missed != schedules.end() && missed->at("status") == "canceled", "A reminder due during paused downtime was left pending");
        restarted.fireDueAutomations(next);
        const auto tasks = taskJson(restarted.tasks(alpha.botId));
        check(tasks.size() == 2 && std::ranges::all_of(tasks,
                                                       [&](const Json& task) {
                                                           const auto& trigger = task.at("trigger");
                                                           return trigger.at("targetTime") == next && trigger.at("occurrenceCount") == 1;
                                                       }),
              "First recurring requests after resume included paused occurrences");
        restarted.fireDueAutomations(next + 15);
        check(restarted.tasks(alpha.botId).size() == 3 && restarted.tasks(alpha.botId).front().task.trigger.value().id == afterResume,
              "A reminder due after resume did not fire");
    }
}

void isolatedFirings(const std::filesystem::path& db) {
    Store store(db);
    const auto now = utc(2026, 3, 1, 1) + 30 * 60;
    const Json rare{{"kind", "schedule"}, {"cron", "30 2 */31,25-31 3 0"}, {"timezone", "Europe/Berlin"}, {"request", "Sparse check"}};
    const auto schedule = store.createAutomation(owner, alpha, rare, utc(2025, 12, 1)).at("id");
    const auto reminder = [&](const Intake& target) {
        return store
            .createAutomation(owner, target,
                              {{"kind", "reminder"}, {"fireAt", now}, {"timezone", "Etc/UTC"}, {"request", "Check the deployment"}},
                              now - 1)
            .at("id");
    };
    const auto invalid = reminder({"broken", "", "env", "protected-destination"});
    const auto regular = reminder(beta);
    auto policy = store.policy();
    const auto secret = policy.requestSecret("broken", {"deployment-token"});
    policy.submitSecret(owner, "broken", secret, "protected-destination");
    store.fireDueAutomations(now);
    const auto alphaSchedule = store.automations(owner, "alpha").at("automations").at(0);
    check(alphaSchedule.at("nextRunAt") == utc(2037, 3, 1, 1) + 30 * 60 && store.tasks("alpha").at(0).task.trigger.value().id == schedule,
          "A long gap after a valid occurrence prevented delivery");
    const auto failed = store.automations(owner, "broken").at("automations").at(0);
    check(failed.at("id") == invalid && failed.at("status") == "failed" && failed.at("nextRunAt").is_null() &&
              store.tasks("broken").empty() && store.tasks("beta").at(0).task.trigger.value().id == regular,
          "A rejected firing blocked or contaminated the rest of the batch");

    const auto end = lastAutomationTime;
    store.createAutomation(
        owner, alpha, {{"kind", "schedule"}, {"cron", "* * * * *"}, {"timezone", "Etc/UTC"}, {"request", "Final occurrence"}}, end - 60);
    store.cancelAutomation(owner, "alpha", schedule.get<Id>());
    store.fireDueAutomations(end);
    check(store.tasks("alpha").size() == 2 && store.automations(owner, "alpha").at("automations").at(0).at("status") == "delivered",
          "The final supported occurrence was dropped");
}

void automationConversation(const std::filesystem::path& db) {
    Store store(db);
    Script bot;
    Adapter adapter;
    RuntimeOptions options;
    options.clock = [] { return utc(2026, 1, 4); };
    Runtime runtime(store, bot, adapter, [](auto) { return std::string{}; }, options);
    bot.choices = {
        {"create_schedule", {{"cron", "0 9 * * 1"}, {"timezone", "Asia/Tokyo"}, {"request", "Weekly progress"}}, "Create schedule"},
        {"create_reminder", {{"fireAt", options.clock() + 600}, {"timezone", "Etc/UTC"}, {"request", "Reminder"}}, "Create reminder"},
        {"create_trigger",
         {{"source", "deploy"}, {"eventFilter", {{"status", "finished"}}}, {"request", "Check deployed status"}},
         "Create trigger"},
        {"list_schedules", Json::object(), "List managed automations"},
        reply(),
    };
    store.receive(localOwner, alpha, "create", "Create my automation");
    runtime.runConversations();
    const auto list = lastResult(bot.inputs.back()).at("automations");
    check(list.size() == 3 && list.at(0).at("createdBy") == "bot:alpha", "Conversation automation tools did not create entries");
    check(bot.inputs.at(1).event.at("currentTime") == options.clock() && list.at(1).at("nextRunAt") == options.clock() + 600,
          "Conversation reminder did not use the Runtime clock");
    const auto triggerId = list.at(0).at("id");
    bot.choices = {{"remove_schedule", {{"id", list.at(2).at("id")}}, "Cancel schedule"}, reply()};
    store.receive(localOwner, alpha, "remove", "Cancel the weekly progress schedule");
    runtime.runConversations();
    check(lastResult(bot.inputs.back()).at("status") == "canceled", "Conversation scheduler could not cancel");
    const auto delivery = store.receiveWebhook(owner, "deploy", "deployment-1", {{"status", "finished"}}, options.clock());
    Store restarted(db);
    const auto replay = restarted.receiveWebhook(owner, "deploy", "deployment-1", {{"status", "finished"}}, options.clock());
    check(replay.at("duplicate") == true && replay.at("tasks") == delivery.at("tasks") && restarted.tasks().size() == 1,
          "Webhook duplicate across connections was enqueued");
    const auto trigger = restarted.tasks().at(0).task.trigger.value();
    const auto& webhook = std::get<WebhookTaskTrigger>(trigger.details);
    check(trigger.id == triggerId.get<Id>() && webhook.source == "deploy" && webhook.event.at("status") == "finished",
          "Webhook trigger provenance was not retained");
}

void webhookPolicy(const std::filesystem::path& db) {
    const Person sender{"alpha-ingress"}, other{"beta-ingress"};
    const auto now = utc(2026, 1, 4);
    const Json trigger{{"kind", "trigger"}, {"source", "deploy"}, {"request", "Check event"}, {"eventFilter", {{"status", "accepted"}}}};
    const Json event{{"status", "accepted"}, {"data", "fixture-webhook-token"}};
    Json alphaReceipt;
    {
        Store store(db);
        auto policy = store.policy();
        policy.setMember(owner, "alpha", sender.id, true);
        policy.setMember(owner, "beta", other.id, true);
        policy.submitSecret(owner, "beta", policy.requestSecret("beta", {"webhook-secret"}), "fixture-webhook-token");
        store.createAutomation(owner, alpha, trigger, now);
        store.createAutomation(owner, beta, trigger, now);
        const auto received = store.receiveWebhook(sender, "deploy", "evt-1", event, now);
        alphaReceipt = received.at("tasks");
        check(received.at("duplicate") == false && alphaReceipt.size() == 1 && alphaReceipt.at(0).at("botId") == "alpha" &&
                  store.tasks("beta").empty(),
              "An unrelated Bot's trigger blocked or received the webhook");
        check(std::get<WebhookTaskTrigger>(store.tasks("alpha").at(0).task.trigger.value().details).event.at("data") == "[redacted]",
              "Webhook fanout copied a protected value into a task");
    }
    Store restarted(db);
    const auto otherDelivery = restarted.receiveWebhook(other, "deploy", "evt-1", event, now);
    check(otherDelivery.at("duplicate") == false && otherDelivery.at("tasks").size() == 1 &&
              otherDelivery.at("tasks").at(0).at("botId") == "beta",
          "A delivery to another Bot consumed this Bot's authorized event");
    const auto replay = restarted.receiveWebhook(sender, "deploy", "evt-1", event, now);
    check(replay.at("duplicate") == true && replay.at("tasks") == alphaReceipt && restarted.tasks().size() == 2,
          "Webhook replay leaked unrelated receipts or queued duplicate work");
}

void pausedWebhookFanout(const std::filesystem::path& db) {
    const auto now = utc(2026, 1, 4);
    const Person sender{"alpha-ingress"};
    const Json trigger{{"kind", "trigger"}, {"source", "deploy"}, {"request", "Check deployment"}};
    const auto deliver = [&](Store& store, const Person& actor, std::string_view eventId) {
        return store.receiveWebhook(actor, "deploy", eventId, Json::object(), now);
    };
    const Json paused = Json::array({alpha.botId});
    Json activeReceipts;
    {
        Store store(db);
        store.policy().setMember(owner, alpha.botId, sender.id, true);
        store.createAutomation(owner, alpha, trigger, now);
        store.createAutomation(owner, beta, trigger, now);
        store.setBotPaused(owner, alpha.botId, true);

        const auto shared = deliver(store, owner, "shared");
        activeReceipts = shared.at("tasks");
        check(shared.at("skippedBots") == paused && activeReceipts.size() == 1 && activeReceipts.front().at("botId") == beta.botId,
              "Paused recipient blocked the active webhook recipient");
        const auto privateEvent = deliver(store, sender, "alpha-only");
        check(privateEvent.at("skippedBots") == paused && privateEvent.at("tasks").empty() &&
                  !privateEvent.at("message").get<std::string>().empty() && store.tasks(alpha.botId).empty(),
              "Webhook to a paused Bot was not refused clearly");
    }
    {
        Store restarted(db);
        restarted.setBotPaused(owner, alpha.botId, false, [&] { return now + 1; });
        const auto shared = deliver(restarted, owner, "shared");
        const auto privateEvent = deliver(restarted, sender, "alpha-only");
        check(shared.at("duplicate") == true && shared.at("skippedBots") == paused && shared.at("tasks") == activeReceipts &&
                  privateEvent.at("duplicate") == true && privateEvent.at("skippedBots") == paused && privateEvent.at("tasks").empty(),
              "A webhook skipped during pause was replayed after restart");
        const auto fresh = deliver(restarted, owner, "fresh");
        check(fresh.at("tasks").size() == 2 && !fresh.contains("skippedBots") && restarted.tasks(alpha.botId).size() == 1 &&
                  restarted.tasks(beta.botId).size() == 2,
              "A fresh webhook did not reach both Bots after resume");
    }
}

void webhookMatchesRawPublicFields(const std::filesystem::path& db) {
    Store store(db);
    const auto now = utc(2026, 1, 1);
    const std::string secret = "fixture-status-token";
    const auto trigger = [&](const std::string& expected) {
        return store
            .createAutomation(
                owner, alpha,
                {{"kind", "trigger"}, {"source", "deploy"}, {"request", "Check deployment"}, {"eventFilter", {{"status", expected}}}}, now)
            .at("id");
    };
    const auto marker = trigger("[redacted]");
    trigger(secret);
    auto policy = store.policy();
    const auto request = policy.requestSecret(alpha.botId, {"status-token"});
    policy.submitSecret(owner, alpha.botId, request, secret);

    const auto hidden = store.receiveWebhook(owner, "deploy", "hidden", {{"status", secret}}, now);
    check(hidden.at("tasks").empty() && store.tasks().empty(), "A protected event field matched a trigger");
    const Json event{{"status", "[redacted]"}, {"details", {{"token", secret}}}};
    const auto visible = store.receiveWebhook(owner, "deploy", "visible", event, now);
    check(visible.at("tasks").size() == 1 && visible.at("tasks").at(0).at("triggerId") == marker,
          "A literal [redacted] event field did not match its trigger");
    const auto task = taskJson(store.tasks(alpha.botId).at(0));
    check(task.at("trigger").at("event").at("details").at("token") == "[redacted]" &&
              store.conversation(alpha.botId).dump().find(secret) == std::string::npos,
          "Webhook provenance retained a protected event value");
}

void webhookRechecksSavedMetadata(const std::filesystem::path& db) {
    Store store(db);
    const auto now = utc(2026, 1, 1);
    const std::string source = "fixture-webhook-source", token = "fixture-request-token";
    constexpr auto payloadText = "I prefer HTML for future release notes.";
    const auto id =
        store
            .createAutomation(owner, alpha,
                              {{"kind", "trigger"}, {"source", source}, {"request", "Check " + token}, {"eventFilter", {{token, "ready"}}}},
                              now)
            .at("id");
    const auto publicId =
        store.createAutomation(owner, alpha, {{"kind", "trigger"}, {"source", source}, {"request", "Check " + token}}, now).at("id");
    auto policy = store.policy();
    policy.submitSecret(owner, beta.botId, policy.requestSecret(beta.botId, {"source-token"}), source);
    policy.submitSecret(owner, beta.botId, policy.requestSecret(beta.botId, {"request-token"}), token);

    const auto list = store.automations(owner, alpha.botId).at("automations");
    check(list.at(1).at("id") == id && list.at(1).at("source") == "[redacted]" && list.at(1).at("request") == "Check [redacted]" &&
              list.at(1).at("eventFilter").contains("[redacted]"),
          "Trigger listing exposed saved webhook metadata that became protected");

    const auto delivery = store.receiveWebhook(owner, source, "evt-1", {{token, "ready"}, {"text", payloadText}}, now);
    const auto receipts = delivery.at("tasks");
    check(receipts.size() == 1 && receipts.at(0).at("triggerId") == publicId,
          "A newly protected filter matched or a redacted trigger failed to deliver");
    const auto taskId = receipts.at(0).at("taskId").get<Id>();
    const auto task = taskJson(store.task(taskId));
    check(task.at("request") == "Check [redacted]" && task.at("intent") == task.at("request") &&
              task.at("trigger").at("source") == "[redacted]",
          "Task retained protected webhook metadata");
    const auto containsMetadata = [&](const Json& data) {
        const auto dump = data.dump();
        return dump.find(source) != dump.npos || dump.find(token) != dump.npos;
    };
    check(!containsMetadata(store.history(taskId)) && !containsMetadata(store.conversation(alpha.botId)),
          "Webhook request or provenance leaked into a journal");

    Adapter adapter;
    Script model;
    RuntimeOptions options;
    options.clock = [&] { return now; };
    Runtime runtime(store, model, adapter, [](auto) { return std::string{}; }, options);
    runtime.tick();
    runtime.tick();
    check(model.inputs.size() == 1 && model.inputs.front().task.at("trigger") == task.at("trigger") &&
              !containsMetadata(model.inputs.front().task) && !containsMetadata(model.inputs.front().event),
          "The task model lost webhook provenance or received protected metadata");
    const auto& input = model.inputs.front();
    check(input.task.at("trigger").at("event").at("text") == payloadText &&
              std::ranges::find(input.tools, "review_preference") == input.tools.end() &&
              std::ranges::find(input.tools, "remember_preference") == input.tools.end() &&
              store.memory(owner, alpha.botId, "memory_history", Json::object()).at("revisions").empty(),
          "A webhook payload can be used as a preference source");

    const auto replay = store.receiveWebhook(owner, source, "evt-1", Json::object(), now);
    check(replay.at("duplicate") == true && replay.at("tasks") == receipts, "Redacted webhook metadata changed delivery deduplication");
}

void webhookEventIdsStayPrivate(const std::filesystem::path& db) {
    Store store(db);
    const auto now = utc(2026, 1, 1);
    const Person sender{"sender", false};
    auto policy = store.policy();
    policy.setMember(owner, alpha.botId, sender.id, true);
    const Json trigger{{"kind", "trigger"}, {"source", "deploy"}, {"request", "Check deployment"}};
    store.createAutomation(owner, alpha, trigger, now);
    store.createAutomation(owner, beta, trigger, now);
    const auto own = policy.requestSecret(alpha.botId, {"alpha-token"});
    const auto other = policy.requestSecret(beta.botId, {"beta-token"});
    policy.submitSecret(owner, alpha.botId, own, "fixture-alpha-secret");
    policy.submitSecret(owner, beta.botId, other, "fixture-beta-secret");

    const auto deliver = [&](std::string_view eventId) { return store.receiveWebhook(sender, "deploy", eventId, Json::object(), now); };
    const std::string_view firstId = "fixture-external-id-1", secondId = "fixture-external-id-2";
    const auto first = deliver(firstId), second = deliver(secondId), replay = deliver(firstId);
    check(first.at("duplicate") == false && second.at("duplicate") == false && replay.at("duplicate") == true &&
              first.at("tasks").size() == 1 && second.at("tasks").size() == 1 && first.at("tasks") != second.at("tasks") &&
              replay.at("tasks") == first.at("tasks") && store.tasks(beta.botId).empty(),
          "Webhook IDs did not deduplicate authorized deliveries independently");
    const auto tasks = taskJson(store.tasks(alpha.botId));
    const auto conversation = store.conversation(alpha.botId).dump();
    check(tasks.size() == 2 && !tasks.at(0).at("trigger").contains("externalEventId") &&
              !tasks.at(1).at("trigger").contains("externalEventId") && conversation.find(firstId) == std::string::npos &&
              conversation.find(secondId) == std::string::npos,
          "Webhook deduplication IDs appeared in task or conversation provenance");
}
}

int main() {
    try {
        Directory temp;
        memoryFlow(temp.path / "memory.sqlite3");
        proactiveMemory(temp.path / "proactive-memory.sqlite3");
        unavailablePreferenceReviewStillReplies(temp.path / "unavailable-review.sqlite3");
        memoryRedactionLimit(temp.path / "memory-limit.sqlite3");
        configuredRequestLimit(temp.path / "schedule-limit.sqlite3");
        downtime(temp.path / "downtime.sqlite3");
        pausedSchedules(temp.path / "paused-schedules.sqlite3");
        isolatedFirings(temp.path / "firings.sqlite3");
        automationConversation(temp.path / "webhook.sqlite3");
        webhookPolicy(temp.path / "webhook-policy.sqlite3");
        pausedWebhookFanout(temp.path / "paused-webhook.sqlite3");
        webhookMatchesRawPublicFields(temp.path / "webhook-filters.sqlite3");
        webhookRechecksSavedMetadata(temp.path / "webhook-saved.sqlite3");
        webhookEventIdsStayPrivate(temp.path / "webhook-metadata.sqlite3");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
