#include "fixtures.hpp"

#include <sqlite3.h>

namespace flockv0::app::test {
namespace {
struct RoutingModel final : Model {
    Decision action;
    Json receipt;

    ModelTurn decide(const ModelInput& input) override {
        if (input.event.at("kind") != "conversation_turn")
            return {{{"delegate",{{"instruction","Verify the release"},{"doneWhen","Report the result"}},"Verify the release."}}};
        const auto& turn = input.event.at("turn");
        if (turn.empty()) return {{action}};
        receipt = turn.back().at("result");
        return {{{"reply",{{"text","I recorded the result."}},"Finish the conversation."}}};
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
        {"local","env-canonical"}};
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

    model.action.args = {{"request","Verify on the Mini"},{"environment","mini"}};
    store.receive(localOwner,intake,"single-verification","Verify on the Mini");
    runtime.runConversations();
    check(model.receipt.value("ok",false) && store.task(model.receipt.at("taskId")).at("projectId") == "mini-project",
          "a single destination must start in the named pinned project");
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
}

void pinnedConversationDestinations() {
    parallelDestinations();
    moveWithProject("mini","mini-project");
    moveWithProject("mini-legacy","remembered-project");
    moveWithProject("mini","mini-project",true);
}
}
