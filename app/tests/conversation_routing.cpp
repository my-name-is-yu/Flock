#include "fixtures.hpp"

#include <sqlite3.h>

namespace flockv0::app::test {
namespace {
struct RoutingModel final : Model {
    Decision action;
    Json receipt;
    Json offeredEnvironments = Json::array();

    ModelTurn decide(const ModelInput& input) override {
        if (input.event.at("kind") != "conversation_turn")
            return {{{"delegate",{{"instruction","Verify the release"},{"doneWhen","Report the result"}},"Verify the release."}}};
        const auto& turn = input.event.at("turn");
        if (turn.empty()) {
            offeredEnvironments = input.task.value("environments",Json::array());
            return {{action}};
        }
        receipt = turn.back().at("result");
        return {{{"reply",{{"text","I recorded the result."}},"Finish the conversation."}}};
    }
};

struct MissingAuthModel final : Model {
    ModelTurn decide(const ModelInput&) override {
        throw ModelError("private provider detail",ModelErrorCode::authentication_required);
    }
};

struct RoutingT3 final : T3 {
    std::vector<Command> commands;
    std::string authGeneration(const std::string&, const Json&) override { return "test-auth"; }
    std::vector<T3Project> projects(std::string_view environment) override {
        check(environment == "mini-env","unexpected project catalog");
        return {{"mini-project","Mini",{}},{"remembered-project","Remembered",{}}};
    }
    Delivery send(const Command& command) override {
        commands.push_back(command);
        return {Delivery::Kind::Accepted,{"thread","run","link"},{}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("Unexpected reconcile"); }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override {
        throw std::runtime_error("Unexpected interrupt");
    }
};

Intake pinnedBot(const Sandbox& temp) {
    auto config = botConfig();
    config["conversationEnvironments"] = {
        {"mini",{{"environmentId","mini-env"},{"projectId","mini-project"}}},
        {"mini-legacy","mini-env"},
        {"local","env-canonical"},
        {"cloud",{{"environmentId","cloud-env"},{"projectId","cloud-project"}}}};
    const auto folder = temp.path / "bots";
    createBot(folder / "release",config,"Verify revisions.","");
    const auto intake = Bots(folder).get("release").intake("");
    check(intake.config.at("conversationEnvironments") == config.at("conversationEnvironments"),
          "Bot config must preserve canonical project aliases and legacy environment aliases");
    return intake;
}

void parallelDestinations() {
    Sandbox temp;
    const auto intake = pinnedBot(temp);
    RoutingModel model;
    RoutingT3 t3;
    Store store(temp.path / "state.db");
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    model.action = {"start_task",{{"request","Verify the release"},{"environments",{"mini","local"}},
                                 {"commit",std::string(40,'A')}},"Use the requested destinations."};
    store.receive(localOwner,intake,"parallel-verification","Verify the release on both Macs");
    runtime.runConversations();
    const auto tasks = store.tasks();
    check(tasks.size() == 1 && t3.commands.empty(),"one conversation request must save exactly one task before dispatch");
    const auto task = store.task(tasks.front().at("id"));
    const Json expected = Json::array({
        {{"name","mini"},{"environmentId","mini-env"},{"projectId","mini-project"}},
        {{"name","local"},{"environmentId","env-canonical"},{"projectId","project-canonical"}}});
    check(task.at("runTargets") == expected && task.at("requestedCommit") == std::string(40,'a'),
          "the task must pin the selected canonical routes and normalized SHA");

    model.action.args["environments"] = {"local","env-canonical"};
    store.receive(localOwner,intake,"duplicate-environment","Verify again on both Macs");
    runtime.runConversations();
    check(store.tasks().size() == 1,"two aliases for one environment must not create a parallel task");

    model.action.args = {{"request","Verify on the Mini"},{"environment","mini"},{"commit",std::string(40,'a')}};
    store.receive(localOwner,intake,"single-verification","Verify on the Mini");
    runtime.runConversations();
    const auto singleNamed = store.task(model.receipt.at("taskId"));
    check(model.receipt.value("ok",false) && singleNamed.at("projectId") == "mini-project" &&
          singleNamed.at("requestedCommit") == std::string(40,'a') && singleNamed.at("runTargets").size() == 1 &&
          singleNamed.at("runTargets").at(0).at("environmentId") == "mini-env",
          "a single named destination must retain optional verification and its saved route");
    model.action.args = {{"request","Verify on the Mini"},{"environments",Json::array({"mini"})}};
    store.receive(localOwner,intake,"single-array","Verify on the Mini");
    runtime.runConversations();
    const auto singleArray = store.task(model.receipt.at("taskId"));
    check(singleArray.at("runTargets").size() == 1 && singleArray.at("runTargets").at(0) ==
          singleNamed.at("runTargets").at(0),"a one-item environments list must route like a named single destination");
    model.action.args = {{"request","Resume the cancelled task"},{"resume",true},
                         {"sourceResultMessageId",1},{"commit",std::string(40,'a')}};
    store.receive(localOwner,intake,"resume-with-commit","Verify the saved work again");
    runtime.runConversations();
    check(!model.receipt.value("ok",true) && model.receipt.dump().find("Commit verification is not supported") != std::string::npos,
          "resume must explicitly reject optional commit verification when its saved destination cannot be verified");
}

void genericDestinationCounts() {
    Sandbox temp;
    const auto intake = pinnedBot(temp);
    RoutingModel model;
    RoutingT3 t3;
    Store store(temp.path / "state.db");
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    model.action = {"start_task",{{"request","Run on the Mini"},{"environments",{"mini"}}},
                    "Use the one selected destination."};
    store.receive(localOwner,intake,"single-route-work","Run this on the Mini");
    runtime.runConversations();
    auto tasks = store.tasks();
    check(tasks.size() == 1,"one destination must create one conversation task");
    const auto single = store.task(tasks.front().at("id"));
    check(single.at("runTargets").size() == 1 && !single.contains("requestedCommit"),
          "ordinary one-destination work must not require commit verification");

    model.action.args = {{"request","Run on all three"},{"environments",{"mini","local","cloud"}}};
    store.receive(localOwner,intake,"three-route-work","Run this on all three destinations");
    runtime.runConversations();
    tasks = store.tasks();
    check(tasks.size() == 2,"three destinations must remain one additional conversation task");
    const auto multi = store.task(tasks.front().at("id"));
    check(multi.at("runTargets").size() == 3 && !multi.contains("requestedCommit"),
          "ordinary multi-destination work must persist all routes without a SHA policy");
}

void manyConfiguredDestinations() {
    Sandbox temp;
    auto config = botConfig();
    Json aliases = Json::object();
    Json selected = Json::array();
    Json expected = Json::array();
    for (int i = 0; i < 17; ++i) {
        const auto suffix = std::to_string(i);
        const auto alias = "destination-" + suffix;
        const auto environment = "environment-" + suffix;
        const auto project = "project-" + suffix;
        aliases[alias] = {{"environmentId",environment},{"projectId",project}};
        selected.push_back(alias);
        expected.push_back({{"name",alias},{"environmentId",environment},{"projectId",project}});
    }
    config["conversationEnvironments"] = std::move(aliases);
    const auto folder = temp.path / "bots";
    createBot(folder / "release",config,"Verify revisions.","");
    const auto intake = Bots(folder).get("release").intake("");
    check(intake.config.at("conversationEnvironments").size() >= 17,
          "Bot registration must retain more than 16 configured destinations");

    RoutingModel model;
    RoutingT3 t3;
    Store store(temp.path / "state.db");
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    model.action = {"start_task",{{"request","Verify on every destination"},{"environments",selected}},
                    "Use all selected destinations."};
    store.receive(localOwner,intake,"seventeen-destinations","Verify on all 17 destinations");
    runtime.runConversations();
    bool allOptionsOffered = model.offeredEnvironments.size() >= 17;
    for (const auto& alias : selected) {
        bool offered = false;
        for (const auto& choice : model.offeredEnvironments)
            offered |= choice.value("name","") == alias.get<std::string>();
        allOptionsOffered &= offered;
    }
    check(allOptionsOffered,"all 17 configured destinations must be offered as choices to one request");
    const auto tasks = store.tasks();
    check(tasks.size() == 1,"one request across 17 destinations must create one task");
    const auto task = store.task(tasks.front().at("id"));
    check(task.at("runTargets") == expected && t3.commands.empty(),
          "one request must save all 17 selected destinations without dropping or dispatching any early");
}

void destinationWithoutSessionIsUnavailable() {
    Sandbox temp;
    auto intake = pinnedBot(temp);
    intake.request = "Verify the release";
    const auto database = temp.path / "state.db";
    Store store(database);
    const auto id = store.submit(intake);
    auto task = store.task(id);
    task["state"] = "running";
    task["runResults"] = Json::array({{{"name","mini"},{"environmentId","mini-env"},
        {"projectId","mini-project"},{"actionId","saved-action"},{"status","running"}}});
    task.erase("runTargets");
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(database.c_str(),&raw);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    check(opened == SQLITE_OK,"could not reopen the task database");
    sqlite3_stmt* rawStatement = nullptr;
    const auto prepared = sqlite3_prepare_v2(db.get(),"UPDATE tasks SET data=? WHERE id=?",-1,&rawStatement,nullptr);
    const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(rawStatement,sqlite3_finalize);
    const auto serialized = task.dump();
    check(prepared == SQLITE_OK && sqlite3_bind_text(statement.get(),1,serialized.c_str(),-1,SQLITE_TRANSIENT) == SQLITE_OK &&
          sqlite3_bind_int64(statement.get(),2,id) == SQLITE_OK && sqlite3_step(statement.get()) == SQLITE_DONE,
          "could not persist the destination Run fixture");

    RoutingModel model;
    RoutingT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    model.action = {"send_to_task",{{"taskId",id},{"generation",task.at("generation")},
        {"instruction","Continue the Mini work"}},"Continue this destination only."};
    store.receive(localOwner,intake,"destination-follow-up","Continue the Mini work");
    runtime.runConversations();
    check(!model.receipt.value("ok",true) &&
          model.receipt.dump().find("destination Session is unavailable") != std::string::npos,
          "a destination without a saved T3 Thread must be rejected as unavailable");
}

void moveWithProject(std::string_view alias, std::string_view project, bool sameEnvironment = false) {
    Sandbox temp;
    auto intake = pinnedBot(temp);
    intake.request = "Verify the release";
    if (sameEnvironment) {
        intake.environmentId = "mini-env";
        intake.projectId = "remembered-project";
    }
    const auto database = temp.path / "state.db";
    Store store(database);
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(database.c_str(),&raw);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    check(opened == SQLITE_OK && sqlite3_exec(db.get(),
          "INSERT INTO bot_t3_projects VALUES('release','mini-env','remembered-project')",
          nullptr,nullptr,nullptr) == SQLITE_OK,"could not save the previous Mini project");

    RoutingModel model;
    RoutingT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    const auto id = store.submit(intake);
    const auto task = store.task(id);
    model.action = {"change_task",{{"taskId",id},{"generation",task.at("generation")},
        {"revisionId",task.value("currentRevision",Id{0})},{"instruction","Move verification to the Mini"},
        {"environment",alias}},"Use the requested Mini destination."};
    store.receive(localOwner,intake,"move-to-mini","Move verification to the Mini");
    runtime.runConversations();
    check(model.receipt.value("ok",false),"change_task rejected the named destination");
    for (int i = 0; i < 12 && t3.commands.empty(); ++i) runtime.tick();
    check(t3.commands.size() == 1 && t3.commands.front().environmentId == "mini-env" &&
          t3.commands.front().projectId == project,
          "the move did not use the requested or remembered project for " + std::string(alias));
}

void missingChatGPTSignInIsActionable() {
    Sandbox temp;
    const auto intake = pinnedBot(temp);
    MissingAuthModel model;
    RoutingT3 t3;
    Store store(temp.path / "state.db");
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    store.receive(localOwner,intake,"missing-chatgpt-sign-in","Check the release");
    runtime.runConversations();
    const auto events = store.conversation("release");
    const auto reply = std::ranges::find_if(events,[](const Json& event) {
        return event.value("kind","") == "conversation_reply";
    });
    check(reply != events.end() && reply->at("data").at("text").get<std::string>().find("Open Flock Settings") != std::string::npos &&
          reply->at("data").at("text").get<std::string>().find("private provider detail") == std::string::npos,
          "missing ChatGPT sign-in did not give a safe, actionable reply");
}
}

void pinnedConversationDestinations() {
    parallelDestinations();
    genericDestinationCounts();
    manyConfiguredDestinations();
    destinationWithoutSessionIsUnavailable();
    moveWithProject("mini","mini-project");
    moveWithProject("mini-legacy","remembered-project");
    moveWithProject("mini","mini-project",true);
    missingChatGPTSignInIsActionable();
}
}
