#include "fixtures.hpp"
#include "../bots.hpp"
#include "flock/automations.hpp"
#include "flock/json.hpp"
#include "flock/memory_automation.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/task_context.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flockv0::app::test {
namespace {
void cronSyntaxAndTimeZones() {
    const auto matches = [](const Cron& cron, Seconds when) { return cron.next(when - 1) == when; };
    const Cron local("0 9 * * 1-5", "Asia/Tokyo");
    check(matches(local, utc(2026, 11, 20, 0, 0)), "weekday in the configured IANA timezone");
    check(!matches(local, utc(2026, 11, 21, 0, 0)), "skip Saturday");
    const Cron listsAndSteps("5,10-22/4 2/3 * 11 7", "Etc/UTC");
    check(matches(listsAndSteps, utc(2026, 11, 22, 5, 14)), "list/range/step/Sunday alias");
    check(!matches(listsAndSteps, utc(2026, 11, 22, 5, 15)), "minute step mismatch");
    const Cron eitherDay("0 0 21 * 1", "Etc/UTC");
    check(matches(eitherDay, utc(2026, 11, 21, 0, 0)), "day-of-month alternative");
    check(matches(eitherDay, utc(2026, 11, 23, 0, 0)), "day-of-week alternative");
    check(!matches(eitherDay, utc(2026, 11, 22, 0, 0)), "neither eligible day");
    rejects<Conflict>([] { Cron("1 2 * * * *", "Etc/UTC"); }, "six fields accepted");
    rejects<Conflict>([] { Cron("*/0 2 * * *", "Etc/UTC"); }, "zero divisor accepted");
    rejects<Conflict>([] { Cron("0 24 * * *", "Etc/UTC"); }, "hour 24 accepted");
    rejects<Conflict>([] { Cron("1,,2 0 * * *", "Etc/UTC"); }, "empty list element accepted");
    rejects<Conflict>([] { Cron("* * * * *", "Not/A_Zone"); }, "unknown time zone accepted");
    rejects<Conflict>([] { Cron("* * * * *", "/usr/share/zoneinfo/UTC"); }, "non-IANA zone path accepted");
    const Cron leap("0 0 29 2 *", "Etc/UTC");
    check(leap.next(utc(2096, 2, 29, 0, 0)) == utc(2104, 2, 29, 0, 0), "century leap year was not skipped");
    const Cron futureDST("30 2 * 3 0", "America/New_York");
    check(futureDST.next(utc(2040, 3, 11, 0, 0)) == utc(2040, 3, 18, 6, 30), "future DST gap was not skipped");
    const Cron halfHourFold("45 1 * * *", "Australia/Lord_Howe");
    const auto firstFold = utc(2026, 4, 4, 14, 45), secondFold = utc(2026, 4, 4, 15, 15);
    const auto folded = halfHourFold.between(firstFold, secondFold);
    check(folded.first == firstFold && folded.last == secondFold && folded.count == 2, "half-hour fall-back lost an instant");
    const Cron always("* * * * *", "Etc/UTC");
    check(always.between(0, utc(2026, 1, 1, 0, 0)).count == utc(2026, 1, 1, 0, 0) / 60 + 1, "multi-decade catch-up lost occurrences");
}

void cronIntakeAndRestart() {
    Sandbox temp;
    const auto folder = temp.path / "bots" / "release";
    auto config = botConfig();
    config["cron"] = Json::array({
        {{"id", "spring"}, {"schedule", "30 2 8 3 *"}, {"timezone", "America/New_York"}, {"request", "Gap"}},
        {{"id", "fall"}, {"schedule", "30 1 1 11 *"}, {"timezone", "America/New_York"}, {"request", "Overlap"}},
    });
    createBot(folder, config, "SOUL", "MEMORY");
    Bots bots(temp.path / "bots");
    const auto db = temp.path / "tasks.db";
    const auto report = [](std::string_view id, std::string_view error) {
        throw std::runtime_error(std::string(id) + ": " + std::string(error));
    };

    {
        Store store(db);
        bots.syncSchedules(store, utc(2026, 3, 8, 6, 0), report);
        Automations(store).fireDueAutomations(utc(2026, 3, 8, 9, 0));
        check(store.tasks().empty(), "nonexistent spring local time was submitted");
        Automations(store).fireDueAutomations(utc(2026, 11, 1, 5, 30));
        Automations(store).fireDueAutomations(utc(2026, 11, 1, 6, 30));
        const auto tasks = store.tasks();
        check(tasks.size() == 2, "both fall-back instants should submit exactly once");
        check(std::get<TimedTaskTrigger>(tasks[0].task.trigger.value().details).targetTime == utc(2026, 11, 1, 6, 30) &&
                  std::get<TimedTaskTrigger>(tasks[1].task.trigger.value().details).targetTime == utc(2026, 11, 1, 5, 30),
              "fall offsets did not produce separate UTC instants");
        check(tasks[0].blockedBy.value().id == tasks[1].task.id, "cron must participate in the same project queue");
    }
    {
        Store store(db);
        bots.syncSchedules(store, utc(2026, 11, 1, 6, 30), report);
        Automations(store).fireDueAutomations(utc(2026, 11, 1, 6, 30));
        check(store.tasks().size() == 2, "duplicate scheduled UTC instant registered after restart");
        Automations(store).fireDueAutomations(utc(2027, 11, 1, 5, 35));
        check(store.tasks().size() == 4, "configured schedules did not retain their due time across restart");
    }
}

void invalidSessionSettingsPreserveDueTimes() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    auto config = botConfig();
    config["cron"] = Json::array({{{"id", "daily"}, {"schedule", "0 9 * * *"}, {"timezone", "Asia/Tokyo"}, {"request", "Daily check"}}});
    createBot(folder, config, "SOUL", "MEMORY");
    const Bots bots(home);
    const auto database = temp.path / "tasks.db";
    const auto start = utc(2026, 1, 1, 12, 0);
    const auto due = utc(2026, 1, 2, 0, 0);
    const auto report = [](std::string_view, std::string_view) {};
    {
        Store store(database);
        bots.syncSchedules(store, start, report);
    }
    config["session"] = {{"workspaceStrategy", {{"kind", "worktree"}}}};
    write(folder / "config.json", config.dump());
    {
        Store store(database);
        bots.syncSchedules(store, due + 3600, report);
    }
    config.erase("session");
    write(folder / "config.json", config.dump());
    Store store(database);
    bots.syncSchedules(store, due + 3600, report);
    Automations(store).fireDueAutomations(due + 3600);
    check(store.tasks("release").size() == 1 &&
              std::get<TimedTaskTrigger>(store.tasks("release").at(0).task.trigger.value().details).targetTime == due,
          "invalid Session settings erased the original scheduled occurrence");
}

void configuredScheduleLifecycle() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    auto config = botConfig();
    const Json entry{{"id", "daily"}, {"schedule", "0 9 * * *"}, {"timezone", "Asia/Tokyo"}, {"request", "Daily check"}};
    config["cron"] = Json::array({entry});
    createBot(folder, config, "SOUL", "MEMORY");
    createBot(home / "other", botConfig(), "", "");
    const Bots bots(home);
    const Person owner{"fixture", true};
    Store store(temp.path / "tasks.db");
    std::vector<std::string> errors;
    const auto sync = [&](Seconds now) {
        bots.syncSchedules(store, now, [&](std::string_view, std::string_view error) { errors.emplace_back(error); });
    };
    const auto schedule = [&] { return Automations(store).automations(owner, "release").at("automations").front(); };
    const auto start = utc(2026, 1, 1, 12, 0);
    const auto next = utc(2026, 1, 2, 0, 0);
    sync(start);
    const auto id = schedule().at("id").get<Id>();
    config["projectId"] = "";
    write(folder / "config.json", config.dump());
    sync(next);
    Automations(store).fireDueAutomations(next);
    check(errors.size() == 1 && schedule().at("status") == "inactive" && store.tasks().empty(),
          "invalid configuration left its previously stored route active");

    config["projectId"] = "repaired-project";
    write(folder / "config.json", config.dump());
    sync(next);
    Automations(store).fireDueAutomations(next);
    check(store.tasks("release").size() == 1 && store.tasks("release").at(0).task.projectId == "repaired-project",
          "repaired config did not restore the schedule's identity and route");

    config["cron"] = Json::array();
    write(folder / "config.json", config.dump());
    sync(next);
    check(schedule().at("status") == "inactive", "omitted schedule was not deactivated");
    config["cron"] = Json::array({entry});
    write(folder / "config.json", config.dump());
    sync(next);
    Automations(store).fireDueAutomations(next);
    check(schedule().at("id") == id && schedule().at("status") == "active" && store.tasks().size() == 1,
          "reintroduced configuration duplicated a firing or lost its schedule ID");
    Automations(store).cancelAutomation(owner, "release", id);
    sync(next);
    check(schedule().at("status") == "canceled", "sync reactivated an explicit cancellation");

    for (const auto* removedBot : {"release", "other"}) {
        const auto routing = bots.get(removedBot).intake("");
        Automations(store).createAutomation(
            owner, routing, {{"kind", "reminder"}, {"timezone", "Asia/Tokyo"}, {"fireAt", next + 1}, {"request", "Check again"}}, next);
        Automations(store).createAutomation(owner, routing, {{"kind", "trigger"}, {"source", "deploy"}, {"request", "Deploy check"}}, next);
        std::filesystem::remove_all(home / removedBot);
    }
    sync(next);
    Automations(store).fireDueAutomations(next + 1);
    rejects<AccessDenied>([&] { Automations(store).receiveWebhook(owner, "deploy", "event-1", Json::object(), next + 1); },
                          "webhook was delivered for a removed Bot");
    check(store.tasks().size() == 1, "automation for a removed Bot queued work");
}

void runSettingsChangesResyncConfiguredSchedules() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    auto config = botConfig();
    config["name"] = "Customer Infrastructure Release Reviewer";
    config["avatar"] = {{"kind", "icon"}, {"icon", "star"}, {"color", "#69c9ac"}};
    config["cron"] = Json::array({
        {{"id", "daily"}, {"schedule", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Daily check"}},
        {{"id", "canceled"}, {"schedule", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Canceled check"}},
    });
    createBot(home / "release", config, "Review releases", "");
    Bots bots(home);
    check(bots.cards().at(0).at("modelCallLimit") == 20, "Bot settings did not show the default allowance");
    Store store(temp.path / "tasks.db");
    const auto start = utc(2026, 1, 1, 12, 0);
    const auto due = utc(2026, 1, 2, 0, 0);
    const Person owner{"fixture", true};
    std::vector<std::string> errors;
    const auto report = [&](std::string_view, std::string_view error) { errors.emplace_back(error); };
    const auto schedules = [&] { return Automations(store).automations(owner, "release").at("automations"); };
    bots.syncSchedules(store, start, report);
    for (const auto& schedule : schedules()) {
        if (schedule.at("configId") == "canceled") Automations(store).cancelAutomation(owner, "release", schedule.at("id"));
    }

    rejects<Conflict>(
        [&] {
            bots.updateRunSettings(store, "release", {{"environmentId", "new-environment"}, {"projectId", "has space"}}, due + 1, report);
        },
        "invalid workspace accepted");
    check(bots.get("release").environmentId == config.at("environmentId").get_ref<const std::string&>(),
          "invalid project partly changed the workspace");
    rejects<BotProfileError>([&] { bots.updateRunSettings(store, "release", {{"environmentId", ""}}, due + 1, report); },
                             "workspace accepted a project with no environment");
    const auto unchanged = bots.get("release");
    check(unchanged.environmentId == config.at("environmentId").get_ref<const std::string&>() &&
              unchanged.projectId == config.at("projectId").get_ref<const std::string&>() && errors.empty(),
          "rejecting an orphaned project changed the workspace or reimported schedules");

    const auto saved =
        bots.updateRunSettings(store, "release", {{"environmentId", "new-environment"}, {"projectId", "new-project"}}, due + 1, report);
    check(saved.at("name") == config.at("name") && saved.at("avatar").at("icon") == "star" && saved.at("soul") == "Review releases" &&
              errors.empty(),
          "workspace save changed an unrelated setting or failed to import schedules");
    const auto moved = bots.get("release");
    check(moved.environmentId == "new-environment" && moved.projectId == "new-project" && moved.settings.at("model") == config.at("model"),
          "workspace save failed to persist the new route or changed the model");
    const auto catalog = Json::array({{{"slug", "next-model"}, {"reasoningEfforts", {"ultra"}}}});
    rejects<BotProfileError>(
        [&] { bots.updateRunSettings(store, "release", {{"model", "next-model"}, {"reasoningEffort", "low"}}, due + 1, report, catalog); },
        "model rejected effort from outside its advertised levels");
    check(bots.get("release").settings.at("model") == config.at("model"), "rejected effort partly changed the model");
    const auto model =
        bots.updateRunSettings(store, "release", {{"model", " next-model "}, {"reasoningEffort", "ultra"}}, due + 1, report, catalog);
    check(model.at("model") == "next-model" && model.at("projectId") == "new-project",
          "model and effort save did not preserve the project");
    const auto allowance = bots.updateRunSettings(store, "release", {{"modelCallLimit", 7}}, due + 1, report);
    check(allowance.at("modelCallLimit") == 7 && allowance.at("model") == "next-model" && allowance.at("reasoningEffort") == "ultra" &&
              allowance.at("projectId") == "new-project",
          "saving the allowance changed the model, effort or destination");
    Automations(store).fireDueAutomations(due + 1);
    const auto tasks = store.tasks("release");
    check(tasks.size() == 1 && tasks.at(0).task.environmentId == "new-environment" && tasks.at(0).task.projectId == "new-project" &&
              std::get<TimedTaskTrigger>(tasks.at(0).task.trigger.value().details).targetTime == due,
          "workspace save lost the due time, kept the old route, or restored a canceled schedule");
    const auto settings = tasks.front().task.config;
    check(settings.at("model") == "next-model" && settings.at("modelProvider") == "chatgpt" && settings.at("reasoningEffort") == "ultra" &&
              settings.at("modelCallLimit") == 7,
          "the scheduled task did not use the saved planner settings");

    const auto cleared = bots.updateRunSettings(store, "release", {{"model", ""}}, due + 1, report);
    std::ifstream savedConfig(home / "release" / "config.json");
    const auto stored = Json::parse(savedConfig);
    check(!cleared.contains("model") && !stored.contains("model") && !stored.contains("modelProvider") &&
              !stored.contains("reasoningEffort") && cleared.at("environmentId") == "new-environment" &&
              cleared.at("projectId") == "new-project" && errors.size() == 1,
          "clearing the model left configuration behind or changed the destination");
    Automations(store).fireDueAutomations(due + 86400);
    check(store.tasks("release").size() == 1, "a configured schedule fired after its model was cleared");
    rejects<BotProfileError>([&] { bots.updateRunSettings(store, "release", {{"reasoningEffort", "low"}}, due + 86400, report); },
                             "a Bot saved an effort without a model");
    bots.updateRunSettings(store, "release", {{"model", "next-model"}}, due + 86400, report);

    const auto unplaced = bots.updateRunSettings(store, "release", {{"projectId", ""}}, due + 86400, report);
    check(!unplaced.contains("projectId") && unplaced.at("environmentId") == "new-environment" && errors.size() == 2,
          "clearing the project changed the environment or silently kept configured schedules active");
    Automations(store).fireDueAutomations(due + 2 * 86400);
    check(store.tasks("release").size() == 1, "a configured schedule fired after its destination was cleared");
}

void automationFiringsUseCurrentPlannerSettings() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    auto original = botConfig("previous-model");
    original["modelCallLimit"] = 9;
    createBot(folder, original, "", "");
    Bots bots(home);
    Store store(temp.path / "tasks.db", {}, [&](std::string_view id) { return bots.plannerModel(id); });
    const Person owner{"fixture", true};
    const auto firstDue = utc(2026, 1, 2, 0, 0);
    const auto nextDue = utc(2026, 1, 3, 0, 0);
    auto saved = bots.get("release").intake("");
    saved.environmentId = "saved-environment";
    saved.projectId = "saved-project";
    saved.config["modelProvider"] = "saved-provider";
    saved.config["reasoningEffort"] = "high";
    saved.config["runTimeoutSeconds"] = 135;
    const auto schedule = Automations(store).createAutomation(
        owner, saved, {{"kind", "schedule"}, {"cron", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Daily check"}}, firstDue - 60);
    const auto trigger = Automations(store).createAutomation(
        owner, saved, {{"kind", "trigger"}, {"source", "deploy"}, {"eventFilter", {{"branch", "main"}}}, {"request", "Deploy check"}},
        firstDue - 60);

    Automations(store).fireDueAutomations(firstDue);
    const auto previousTask = store.tasks("release").at(0).task.id;
    const auto report = [](std::string_view, std::string_view error) { throw std::runtime_error(std::string(error)); };
    bots.updateRunSettings(store, "release",
                           {{"model", "current-model"},
                            {"environmentId", "current-environment"},
                            {"projectId", "current-project"},
                            {"reasoningEffort", "low"},
                            {"modelCallLimit", 7}},
                           firstDue, report);

    Automations(store).fireDueAutomations(nextDue);
    const auto recurring = store.tasks("release").at(0).task;
    check(recurring.trigger.value().id == schedule.at("id").get<Id>() &&
              std::get<TimedTaskTrigger>(recurring.trigger.value().details).targetTime == nextDue,
          "model change lost the next scheduled occurrence");
    const auto webhook = Automations(store).receiveWebhook(owner, "deploy", "event-1", {{"branch", "main"}}, nextDue);
    check(webhook.at("tasks").size() == 1 && webhook.at("tasks").at(0).at("triggerId") == trigger.at("id"),
          "model change lost the matched webhook firing");

    auto currentConfig = saved.config;
    currentConfig["model"] = "current-model";
    currentConfig["modelProvider"] = "chatgpt";
    currentConfig["modelCallLimit"] = 7;
    currentConfig["reasoningEffort"] = "low";
    const auto checkTask = [&](Id id, const Json& expected) {
        const auto task = store.task(id).task;
        check(task.config == expected && task.environmentId == saved.environmentId && task.projectId == saved.projectId,
              "automation task used the wrong planner settings or lost its saved route");
    };
    checkTask(recurring.id, currentConfig);
    checkTask(webhook.at("tasks").at(0).at("taskId").get<Id>(), currentConfig);
    auto previousConfig = currentConfig;
    previousConfig["model"] = "previous-model";
    previousConfig["modelCallLimit"] = 9;
    previousConfig.erase("reasoningEffort");
    checkTask(previousTask, previousConfig);

    const auto defaultDue = utc(2026, 1, 4, 0, 0);
    bots.updateRunSettings(store, "release", {{"reasoningEffort", ""}}, nextDue, report);
    Automations(store).fireDueAutomations(defaultDue);
    currentConfig.erase("reasoningEffort");
    checkTask(store.tasks("release").at(0).task.id, currentConfig);

    write(folder / "config.json", botConfig("current-model").dump());
    const auto reset = Automations(store).receiveWebhook(owner, "deploy", "event-2", {{"branch", "main"}}, defaultDue);
    currentConfig.erase("modelCallLimit");
    checkTask(reset.at("tasks").at(0).at("taskId").get<Id>(), currentConfig);

    const auto fallbackDue = utc(2026, 1, 5, 0, 0);
    write(folder / "config.json", "invalid");
    Automations(store).fireDueAutomations(fallbackDue);
    const auto fallback = store.tasks("release").at(0).task;
    check(std::get<TimedTaskTrigger>(fallback.trigger.value().details).targetTime == fallbackDue,
          "unreadable Bot settings prevented the saved automation from firing");
    checkTask(fallback.id, saved.config);
}

void unreadableProfileKeepsAutomationDelivery() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    createBot(home / "release", botConfig("release-saved"), "", "");
    createBot(home / "triage", botConfig("triage-saved"), "", "");
    Bots bots(home);
    Store store(temp.path / "tasks.db", {}, [&](std::string_view id) { return bots.plannerModel(id); });
    const Person owner{"fixture", true};
    const auto due = utc(2026, 1, 2, 0, 0);
    const Json schedule = {{"kind", "schedule"}, {"cron", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Daily check"}};
    const Json trigger = {{"kind", "trigger"}, {"source", "deploy"}, {"eventFilter", {{"branch", "main"}}}, {"request", "Deploy check"}};
    auto release = bots.get("release").intake("");
    release.config["modelProvider"] = "saved-provider";
    for (const auto& bot : {release, bots.get("triage").intake("")}) {
        Automations(store).createAutomation(owner, bot, schedule, due - 60);
        Automations(store).createAutomation(owner, bot, trigger, due - 60);
    }
    write(home / "triage" / "config.json", botConfig("triage-current").dump());
    std::filesystem::rename(home / "release", home / "release.saved");
    std::filesystem::create_symlink("release", home / "release");

    Automations(store).fireDueAutomations(due);
    Automations(store).fireDueAutomations(due);
    const auto delivered = Automations(store).receiveWebhook(owner, "deploy", "event-1", {{"branch", "main"}}, due);
    const auto repeated = Automations(store).receiveWebhook(owner, "deploy", "event-1", {{"branch", "main"}}, due);
    check(delivered.at("tasks").size() == 2 && repeated.at("duplicate") == true && repeated.at("tasks") == delivered.at("tasks"),
          "both Bots must receive one copy of the webhook");
    for (const auto id : {"release", "triage"}) {
        const bool unavailable = std::string_view(id) == "release";
        const auto tasks = store.tasks(id);
        check(tasks.size() == 2 && tasks.at(0).task.request == "Deploy check" && tasks.at(1).task.request == "Daily check",
              "both Bots must receive one task per automation");
        for (const auto& task : tasks) {
            const auto settings = task.task.config;
            check(settings.at("model") == (unavailable ? "release-saved" : "triage-current") &&
                      settings.at("modelProvider") == (unavailable ? "saved-provider" : "chatgpt"),
                  "automations must use the saved planner only when the current profile is unavailable");
        }
    }
}

void failedScheduleSyncLeavesWorkspaceUnchanged() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto database = temp.path / "tasks.db";
    auto config = botConfig();
    config["cron"] = Json::array({
        {{"id", "daily"}, {"schedule", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Daily check"}},
    });
    createBot(home / "release", config, "", "");
    Bots bots(home);
    Store store(database);
    const auto due = utc(2026, 1, 2, 0, 0);
    const auto report = [](std::string_view, std::string_view error) { throw std::runtime_error(std::string(error)); };
    bots.syncSchedules(store, due - 60, report);

    sqlite3* handle = nullptr;
    const auto opened = sqlite3_open(database.c_str(), &handle);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(handle, sqlite3_close);
    check(opened == SQLITE_OK && sqlite3_exec(connection.get(), R"SQL(
CREATE TRIGGER reject_schedule_routing BEFORE UPDATE OF routing ON automations
BEGIN SELECT RAISE(ABORT,'schedule write rejected'); END;
)SQL",
                                              nullptr, nullptr, nullptr) == SQLITE_OK,
          "could not reject the schedule route write");

    rejects<std::runtime_error>([&] { bots.updateRunSettings(store, "release", {{"projectId", "next-project"}}, due, report); },
                                "workspace save succeeded after the schedule write failed");
    Automations(store).fireDueAutomations(due);
    const auto tasks = store.tasks("release");
    const auto& project = config.at("projectId").get_ref<const std::string&>();
    check(tasks.size() == 1 && tasks.front().task.projectId == project && bots.get("release").projectId == project,
          "a failed schedule update published a destination that did not match the due task");
}

void invalidEntriesPreserveDueSchedule() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    const auto db = temp.path / "tasks.db";
    const Bots bots(home);
    auto config = botConfig();
    config["cron"] = Json::array({
        {{"id", "daily"}, {"schedule", "0 9 * * *"}, {"timezone", "Asia/Tokyo"}, {"request", "Daily check"}},
        {{"id", "report"}, {"schedule", "0 9 * * *"}, {"timezone", "Asia/Tokyo"}, {"request", "Write report"}},
        {{"id", "review"}, {"schedule", "0 9 * * *"}, {"timezone", "Asia/Tokyo"}, {"request", "Review report"}},
    });
    createBot(folder, config, "", "");
    std::vector<std::string> errors;
    const auto report = [&](std::string_view, std::string_view error) { errors.emplace_back(error); };
    {
        Store store(db);
        bots.syncSchedules(store, utc(2026, 1, 1, 12, 0), report);
        check(errors.empty(), "Initial schedule import failed");
    }

    config["cron"][1]["timezone"] = "Not/A_Zone";
    config["cron"][2]["request"] = std::string(16'001, 'x');
    config["cron"].push_back(nullptr);
    write(folder / "config.json", config.dump());
    Store store(db);
    const auto now = utc(2026, 1, 4, 12, 0);
    const auto firstDue = utc(2026, 1, 2, 0, 0);
    bots.syncSchedules(store, now, report);
    check(errors.size() == 3 && errors[0].starts_with("cron schedule report:") && errors[1] == "cron schedule review: Invalid request" &&
              errors[2].starts_with("cron entry 4:"),
          "Invalid cron entries were not reported individually");
    const auto schedules = Automations(store).automations({"fixture", true}, "release").at("automations");
    check(schedules.at(0).at("status") == "inactive" && schedules.at(1).at("status") == "inactive" &&
              schedules.at(2).at("status") == "active" && schedules.at(2).at("nextRunAt") == firstDue,
          "Invalid cron edits disturbed the valid sibling's saved run time");
    Automations(store).fireDueAutomations(now);
    const auto tasks = store.tasks("release");
    check(tasks.size() == 1 && tasks.at(0).task.request == "Daily check" &&
              std::get<TimedTaskTrigger>(tasks.at(0).task.trigger.value().details).coveredFrom == firstDue &&
              std::get<TimedTaskTrigger>(tasks.at(0).task.trigger.value().details).occurrenceCount == 3,
          "The valid sibling did not fire its saved occurrences after restart");
}

void malformedDuplicatesDeactivateSchedules() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    const auto db = temp.path / "tasks.db";
    const Bots bots(home);
    auto config = botConfig();
    const Json daily{{"id", "daily"}, {"schedule", "0 0 * * *"}, {"timezone", "Etc/UTC"}, {"request", "Daily check"}};
    const Json weekly{{"id", "weekly"}, {"schedule", "0 0 * * 5"}, {"timezone", "Etc/UTC"}, {"request", "Weekly check"}};
    config["cron"] = Json::array({daily, weekly});
    createBot(folder, config, "", "");
    std::vector<std::string> errors;
    const auto report = [&](std::string_view, std::string_view error) { errors.emplace_back(error); };
    {
        Store store(db);
        bots.syncSchedules(store, utc(2026, 1, 1, 12, 0), report);
        check(errors.empty(), "Initial schedule import failed");
    }

    auto malformed = daily;
    malformed.erase("request");
    config["cron"] = Json::array({malformed, weekly, daily});
    write(folder / "config.json", config.dump());
    Store store(db);
    const auto due = utc(2026, 1, 2, 0, 0);
    bots.syncSchedules(store, due, report);
    check(errors ==
              std::vector<std::string>{"cron entry 1: Duplicate configured schedule ID", "cron entry 3: Duplicate configured schedule ID"},
          "A malformed duplicate did not invalidate both entries");
    const auto schedules = Automations(store).automations({"fixture", true}, "release").at("automations");
    check(schedules.size() == 2 && schedules.at(1).at("configId") == "daily" && schedules.at(1).at("status") == "inactive" &&
              schedules.at(1).at("nextRunAt").is_null(),
          "A malformed duplicate left the previously configured schedule active");
    Automations(store).fireDueAutomations(due);
    const auto tasks = store.tasks("release");
    check(tasks.size() == 1 && tasks.at(0).task.request == "Weekly check", "The unrelated schedule did not fire on restart");
}
}

void configuredSchedules() {
    cronSyntaxAndTimeZones();
    cronIntakeAndRestart();
    configuredScheduleLifecycle();
    runSettingsChangesResyncConfiguredSchedules();
    automationFiringsUseCurrentPlannerSettings();
    unreadableProfileKeepsAutomationDelivery();
    failedScheduleSyncLeavesWorkspaceUnchanged();
    invalidSessionSettingsPreserveDueTimes();
    invalidEntriesPreserveDueSchedule();
    malformedDuplicatesDeactivateSchedules();
}
}
