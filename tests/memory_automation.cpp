#include "flock/access.hpp"
#include "flock/memory_automation.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <future>
#include <iostream>
#include <unistd.h>

using namespace flockv0;
namespace {
const Person owner{"owner",true};
const Intake alpha{"alpha","","env","alpha-project"}, beta{"beta","","env","beta-project"};

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        char name[] = "/tmp/flock-memory-XXXXXX";
        const auto made = mkdtemp(name);
        check(made != nullptr,"Temporary directory creation failed");
        path = made;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); }
};

struct Adapter : T3 {
    std::string authGeneration(const std::string&,const Json&) override { return {}; }
    Delivery send(const Command&) override { throw std::runtime_error("Unexpected Session launch"); }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
    void interrupt(const std::string&,const Json&,const RunRef&) override {}
};

struct Script : Model {
    std::vector<Decision> choices;
    std::vector<ModelInput> inputs;
    Decision decide(const ModelInput& input) override {
        inputs.push_back(input);
        if (!choices.empty()) {
            check(input.tools.end() != std::find(input.tools.begin(),input.tools.end(),choices.front().tool),
                  "Conversation tool was not advertised");
            auto choice = std::move(choices.front());
            choices.erase(choices.begin());
            return choice;
        }
        return {"ask_user",{{"question","Anything else?"}},"Fixture task"};
    }
};

Decision reply() { return {"reply",{{"text","Done"}},"Confirmed"}; }

const Json& lastResult(const ModelInput& input) {
    return input.event.at("turn").back().at("result");
}

void memoryFlow(const std::filesystem::path& db) {
    Store store(db);
    auto access = store.access();
    const std::string path = "team/staging.md";
    for (const auto* bot : {"alpha","beta"})
        for (const auto* op : {"read","write"})
            access.resourceGrant(owner,bot,{"shared_memory",path,op},GrantChange::grant);

    Adapter adapter;
    Script bot;
    RuntimeOptions options; options.clock = [] { return Seconds{1'767'456'000}; };
    Runtime runtime(store,bot,adapter,[](std::string_view id) {
        return BotNotes{"",id == "alpha" ? "Prefer CSV.\n" : "Beta private."};
    },options);
    const auto turn = [&](const Intake& target, std::string_view id, std::vector<Decision> choices) {
        const auto start = bot.inputs.size();
        choices.push_back(reply());
        bot.choices = std::move(choices);
        store.receive(target,id,id);
        runtime.runConversations();
        check(bot.choices.empty(),"Conversation did not complete");
        return start;
    };
    const auto step = turn(alpha,"Remember to use staging-eu for staging checks",{
        {"read_memory",Json::object(),"Read current conventions"},
        {"propose_memory_edit",{{"baseVersion",0},{"content","Prefer CSV.\nUse staging-eu.\n"},
                                 {"rationale","User specified the staging environment"}},"Propose convention"},
    });
    check(lastResult(bot.inputs.at(step+1)).at("version") == 0 &&
          lastResult(bot.inputs.at(step+1)).at("content") == "Prefer CSV.\n","Private memory did not come from notes");
    const auto proposal = lastResult(bot.inputs.at(step+2)).at("proposalId");
    const auto apply = turn(alpha,"Apply the convention",{
        {"apply_memory_edit",{{"baseVersion",0},{"proposalId",proposal}},"Apply"},
        {"memory_history",Json::object(),"Verify change"},
    });
    const auto history = lastResult(bot.inputs.at(apply+2));
    const auto first = history.at("revisions").at(0);
    check(first.at("author") == "bot:alpha" && first.at("diffSummary") == "-0 +1 lines" &&
          history.at("proposals").at(0).at("status") == "applied","Memory proposal and diff were not recorded");
    const auto followup = turn(alpha,"Which staging environment is standard?",{});
    check(bot.inputs.at(followup).notes.memory == "Prefer CSV.\nUse staging-eu.\n","BotNotes did not load edited memory");

    store.memory(owner,"alpha","revert_memory_edit",{{"baseVersion",1},{"revisionId",first.at("id")}});
    const auto restored = store.memory(owner,"alpha","read_memory",Json::object());
    const auto revisions = store.memory(owner,"alpha","memory_history",Json::object()).at("revisions");
    check(restored.at("content") == "Prefer CSV.\n" && restored.at("version") == 2 && revisions.size() == 2 &&
          revisions.at(0).at("revertedFrom") == first.at("id") && revisions.at(0).at("author") == "person:owner",
          "Revert did not preserve the original revision and restore content");

    const auto shared = turn(alpha,"Update the staging playbook",{
        {"apply_memory_edit",{{"docId",path},{"baseVersion",0},{"content","Staging playbook"}},"Update shared file"},
    });
    check(lastResult(bot.inputs.at(shared+1)).at("ok") == true,"Writable shared file denied");
    const auto reader = turn(beta,"Read the staging playbook",{
        {"read_memory",{{"docId",path}},"Read allowed file"},
        {"read_memory",Json::object(),"Read my private memory"},
        {"read_memory",{{"docId","bot:alpha:memory"}},"Try another Bot's private key"},
    });
    check(lastResult(bot.inputs.at(reader+1)).at("content") == "Staging playbook" &&
          lastResult(bot.inputs.at(reader+2)).at("content") == "Beta private." &&
          lastResult(bot.inputs.at(reader+3)).at("ok") == false,"Shared or private memory boundary violated");
    access.resourceGrant(owner,"beta",{"shared_memory","bot:alpha:memory","read"},GrantChange::grant);
    const auto spoof = turn(beta,"Inspect the granted shared key",{
        {"read_memory",{{"docId","bot:alpha:memory"}},"Read shared key"},
    });
    check(lastResult(bot.inputs.at(spoof+1)).at("docKey") == "shared:bot:alpha:memory" &&
          lastResult(bot.inputs.at(spoof+1)).at("content") == "","Grant to a shared key exposed private memory");
    const Intake gamma{"gamma","","env","gamma-project"};
    const auto denied = turn(gamma,"Try the staging playbook",{
        {"read_memory",{{"docId",path}},"Try unauthorized read"},
        {"apply_memory_edit",{{"docId",path},{"baseVersion",1},{"content","Unauthorized"}},"Try unauthorized write"},
    });
    check(lastResult(bot.inputs.at(denied+1)).at("ok") == false &&
          lastResult(bot.inputs.at(denied+2)).at("ok") == false,"Ungranted Bot accessed a shared file");

    Store left(db), right(db);
    std::barrier start(2);
    const auto personEdit = [&](Store& connection, const Person& person, const Intake& target, std::string_view content) {
        start.arrive_and_wait();
        try {
            connection.memory(person,target.botId,"apply_memory_edit",{{"docId",path},{"baseVersion",1},{"content",content}});
            return true;
        } catch (const Conflict& error) {
            check(std::string_view(error.what()).starts_with("Memory version conflict"),"Wrong concurrent edit failure");
            return false;
        }
    };
    const Person editor{"editor",false};
    access.personGrant(owner,"beta",editor.id,Role::decide,GrantChange::grant);
    access.personScope(owner,editor.id,{"shared_memory",path,"write"},GrantChange::grant);
    auto firstEdit = std::async(std::launch::async,[&] { return personEdit(left,owner,alpha,"Alpha version"); });
    auto secondEdit = std::async(std::launch::async,[&] { return personEdit(right,editor,beta,"Beta version"); });
    const auto alphaWon = firstEdit.get(), betaWon = secondEdit.get();
    check(alphaWon != betaWon,"Concurrent edits did not have exactly one winner");
    const auto final = store.memory(owner,"alpha","read_memory",{{"docId",path}});
    check(final.at("version") == 2 && final.at("content") == (alphaWon ? "Alpha version" : "Beta version"),
          "A concurrent shared edit silently overwrote the winner");
    const auto stale = turn(beta,"Try a stale shared edit",{
        {"apply_memory_edit",{{"docId",path},{"baseVersion",1},{"content","Stale edit"}},"Apply stale snapshot"},
    });
    check(lastResult(bot.inputs.at(stale+1)).at("error").get<std::string>().starts_with("Memory version conflict"),
          "Bot accepted a stale shared version");
}

Seconds utc(int year, unsigned month, unsigned day, int hour = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>((sys_days{std::chrono::year{year}/month/day} + hours{hour}).time_since_epoch()).count();
}

void downtime(const std::filesystem::path& db) {
    auto now = utc(2026,1,4);
    const auto first = utc(2026,1,5), last = utc(2026,1,19), next = utc(2026,1,26);
    const auto fireAt = utc(2026,1,6,7);
    Id summaryId, reminderId, canceledId;
    const Json weekly{{"kind","schedule"},{"cron","0 9 * * 1"},{"timezone","Asia/Tokyo"},{"request","Weekly summary"}};
    {
        Store store(db);
        summaryId = store.createAutomation(owner,alpha,weekly,now).at("id");
        reminderId = store.createAutomation(owner,beta,{{"kind","reminder"},{"timezone","Asia/Tokyo"},
            {"fireAt",fireAt},{"label","Staging"},{"request","Check staging now"}},now).at("id");
        canceledId = store.createAutomation(owner,beta,weekly,now).at("id");
        store.cancelAutomation(owner,beta.botId,canceledId);
        store.fireDueAutomations(now);
        check(store.tasks().empty(),"Automation fired before its target time");
    }
    now = utc(2026,1,20,4);
    const auto tick = [&](Store& store) {
        Script model;
        Adapter adapter;
        RuntimeOptions options; options.clock = [&] { return now; };
        Runtime runtime(store,model,adapter,[](auto) { return BotNotes{}; },options);
        runtime.tick();
    };
    {
        Store store(db);
        tick(store);
        const auto tasks = store.tasks();
        check(tasks.size() == 2,"Downtime did not produce exactly two queued requests");
        const auto recurring = store.tasks("alpha").at(0).at("trigger");
        const auto reminder = store.tasks("beta").at(0).at("trigger");
        check(recurring.at("triggerId") == summaryId && recurring.at("triggerKind") == "schedule" &&
              recurring.at("coveredFrom") == first && recurring.at("coveredTo") == last &&
              recurring.at("occurrenceCount") == 3 && recurring.at("targetTime") == last,
              "Weekly occurrences were not coalesced into one covered interval");
        check(reminder.at("triggerId") == reminderId && reminder.at("triggerKind") == "reminder" &&
              reminder.at("targetTime") == fireAt && reminder.at("occurrenceCount") == 1,
              "Missed reminder lost its provenance");
        const auto botSchedules = store.automations(owner,"beta").at("automations");
        check(botSchedules.at(0).at("id") == canceledId && botSchedules.at(0).at("status") == "canceled" &&
              botSchedules.at(1).at("status") == "delivered","Reminder or canceled schedule status incorrect");
        check(store.automations(owner,"alpha").at("automations").at(0).at("nextRunAt") == next,
              "Next weekly summary does not use its configured timezone");
    }
    {
        Store store(db);
        tick(store);
        check(store.tasks().size() == 2,"Restart at identical time fired again");
        now = next - 1;
        tick(store);
        check(store.tasks().size() == 2,"Next weekly summary fired early");
        now = next;
        tick(store);
        tick(store);
        check(store.tasks().size() == 3 && store.tasks("alpha").at(0).at("trigger").at("targetTime") == next &&
              store.tasks("alpha").at(0).at("trigger").at("occurrenceCount") == 1,"Next weekly occurrence did not fire once");
    }
}

void automationConversation(const std::filesystem::path& db) {
    Store store(db);
    Script bot;
    Adapter adapter;
    RuntimeOptions options; options.clock = [] { return utc(2026,1,4); };
    Runtime runtime(store,bot,adapter,[](auto) { return BotNotes{}; },options);
    bot.choices = {
        {"create_schedule",{{"cron","0 9 * * 1"},{"timezone","Asia/Tokyo"},{"request","Weekly progress"}},"Create schedule"},
        {"create_reminder",{{"fireAt",utc(2026,1,6)},{"timezone","Etc/UTC"},{"request","Reminder"}},"Create reminder"},
        {"create_trigger",{{"source","deploy"},{"eventFilter",{{"status","finished"}}},{"request","Check deployed status"}},"Create trigger"},
        {"list_schedules",Json::object(),"List managed automations"},reply(),
    };
    store.receive(alpha,"create","Create my automation");
    runtime.runConversations();
    const auto list = lastResult(bot.inputs.back()).at("automations");
    check(list.size() == 3 && list.at(0).at("createdBy") == "bot:alpha","Conversation automation tools did not create entries");
    const auto triggerId = list.at(0).at("id");
    bot.choices = {{"remove_schedule",{{"id",list.at(2).at("id")}},"Cancel schedule"},reply()};
    store.receive(alpha,"remove","Cancel the weekly progress schedule");
    runtime.runConversations();
    check(lastResult(bot.inputs.back()).at("status") == "canceled","Conversation scheduler could not cancel");
    const auto delivery = store.receiveWebhook(owner,"deploy","deployment-1",{{"status","finished"}},options.clock());
    Store restarted(db);
    const auto replay = restarted.receiveWebhook(owner,"deploy","deployment-1",{{"status","finished"}},options.clock());
    check(replay.at("duplicate") == true && replay.at("tasks") == delivery.at("tasks") &&
          restarted.tasks().size() == 1,"Webhook duplicate across connections was enqueued");
    const auto trigger = restarted.tasks().at(0).at("trigger");
    check(trigger.at("triggerId") == triggerId && trigger.at("triggerKind") == "webhook" &&
          trigger.at("externalEventId") == "deployment-1" && trigger.at("event").at("status") == "finished",
          "Webhook trigger provenance was not retained");
}
}

int main() {
    try {
        Directory temp;
        memoryFlow(temp.path / "memory.sqlite3");
        downtime(temp.path / "downtime.sqlite3");
        automationConversation(temp.path / "webhook.sqlite3");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
